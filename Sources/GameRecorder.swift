import Foundation
import AVFoundation
import CoreMedia
import CoreVideo
import Photos
import VideoToolbox

// 録画（VR_CAP_RECORD・OBS の録画を iPhone で・道 A）の iPhone 側の書き手。
// 受け手（vk/remote/server.c が取り込む record.h）の録画の糸から呼ばれる。絵は窓に出すコマ（CPU から見える BGRA）、
// 音は口 50063（スピーカーの係の AudioBridge）で届いた PC の音。AVAssetWriter で HEVC（だめなら H.264）と AAC の mp4 に書く。
// ファイルは Documents/Recordings/名前.mp4。PC のドライバが VR_RECORD_FETCH で持ち帰れる。頼まれたら「写真」にも入れる。
//
// まだ一度も組んでいない（Windows では iPhone 用に組めない）。Apple の資料どおりに書いた。最初のエラーは
// CMSampleBuffer を作る所（音）と、C の関数の型の所（@convention(c) の引数）から出る見込み。
//
// 足し方：RpcServerApp.swift の FXUpscaler.register() の次の行に GameRecorder.register()。
//        vk_bridge.h に ios/vk_bridge_record.h の中身を足す。AudioBridge.swift の SPK を受ける所に 1 行（ios/AudioBridge.patch.md）
final class GameRecorder {
    private let writer: AVAssetWriter
    private let vin: AVAssetWriterInput
    private let adaptor: AVAssetWriterInputPixelBufferAdaptor
    private let ain: AVAssetWriterInput?
    private var afmt: CMAudioFormatDescription?
    let url: URL
    private let toPhotos: Bool
    private let w: Int, h: Int
    private var aNext: CMTime? // 音の次の時刻。届いた時刻が少しずれても詰めて並べる（隙間と重なりを作らない）
    private(set) var dropped = 0

    private init?(name: String, w: Int, h: Int, fps: Int, hevc: Bool, kbps: Int, arate: Int, ach: Int, akbps: Int, toPhotos: Bool) {
        let fm = FileManager.default
        guard let docs = fm.urls(for: .documentDirectory, in: .userDomainMask).first else { return nil }
        let dir = docs.appendingPathComponent("Recordings", isDirectory: true)
        try? fm.createDirectory(at: dir, withIntermediateDirectories: true)
        url = dir.appendingPathComponent(name + ".mp4")
        try? fm.removeItem(at: url)
        guard let wr = try? AVAssetWriter(outputURL: url, fileType: .mp4) else { return nil }
        writer = wr
        self.w = w
        self.h = h
        self.toPhotos = toPhotos
        // 映像：録画向けの設定。遅れは気にしないので並べ替え（B フレーム）を許し、キーフレームは 2 秒ごと
        func settings(_ codec: AVVideoCodecType, _ profile: String) -> [String: Any] {
            [AVVideoCodecKey: codec, AVVideoWidthKey: w, AVVideoHeightKey: h,
             AVVideoCompressionPropertiesKey: [AVVideoAverageBitRateKey: kbps * 1000,
                                               AVVideoExpectedSourceFrameRateKey: fps,
                                               AVVideoMaxKeyFrameIntervalDurationKey: 2.0,
                                               AVVideoAllowFrameReorderingKey: true,
                                               AVVideoProfileLevelKey: profile] as [String: Any]]
        }
        var vs = settings(.h264, AVVideoProfileLevelH264HighAutoLevel)
        if hevc {
            let hs = settings(.hevc, kVTProfileLevel_HEVC_Main_AutoLevel as String)
            if wr.canApply(outputSettings: hs, forMediaType: .video) { vs = hs }
        }
        let vi = AVAssetWriterInput(mediaType: .video, outputSettings: vs)
        vi.expectsMediaDataInRealTime = true
        guard wr.canAdd(vi) else { return nil }
        wr.add(vi)
        vin = vi
        adaptor = AVAssetWriterInputPixelBufferAdaptor(assetWriterInput: vi, sourcePixelBufferAttributes: [
            kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_32BGRA,
            kCVPixelBufferWidthKey as String: w, kCVPixelBufferHeightKey as String: h,
            kCVPixelBufferIOSurfacePropertiesKey as String: [String: Any]()])
        // 音：AAC（16 ビットの PCM から AVAssetWriter が縮める）
        if arate > 0 {
            let asSettings: [String: Any] = [AVFormatIDKey: kAudioFormatMPEG4AAC, AVSampleRateKey: arate,
                                             AVNumberOfChannelsKey: ach, AVEncoderBitRateKey: akbps * 1000]
            let ai = AVAssetWriterInput(mediaType: .audio, outputSettings: asSettings)
            ai.expectsMediaDataInRealTime = true
            if wr.canAdd(ai) { wr.add(ai); ain = ai } else { ain = nil }
            var asbd = AudioStreamBasicDescription(mSampleRate: Float64(arate), mFormatID: kAudioFormatLinearPCM,
                                                   mFormatFlags: kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked,
                                                   mBytesPerPacket: UInt32(2 * ach), mFramesPerPacket: 1, mBytesPerFrame: UInt32(2 * ach),
                                                   mChannelsPerFrame: UInt32(ach), mBitsPerChannel: 16, mReserved: 0)
            CMAudioFormatDescriptionCreate(allocator: kCFAllocatorDefault, asbd: &asbd, layoutSize: 0, layout: nil,
                                           magicCookieSize: 0, magicCookie: nil, extensions: nil, formatDescriptionOut: &afmt)
        } else {
            ain = nil
        }
        guard wr.startWriting() else { return nil }
        wr.startSession(atSourceTime: .zero) // 受け手は最初に書けたコマを 0 にした時刻で渡す
        let codecName = (vs[AVVideoCodecKey] as? AVVideoCodecType) == .hevc ? "HEVC" : "H.264"
        vkr_note("録画：\(url.lastPathComponent)・\(w)x\(h)・\(codecName) \(kbps) kbps・音 \(arate) Hz")
    }

    // 絵 1 枚。書き手が受け付けない間（isReadyForMoreMediaData が偽）は落とす。1 なら続けてよい
    private func video(_ px: UnsafePointer<UInt8>, w: Int, h: Int, stride: Int, bgra: Bool, ptsUs: Int64) -> Bool {
        if writer.status == .failed { return false }
        guard vin.isReadyForMoreMediaData, let pool = adaptor.pixelBufferPool else { dropped += 1; return true }
        var pbo: CVPixelBuffer?
        guard CVPixelBufferPoolCreatePixelBuffer(nil, pool, &pbo) == kCVReturnSuccess, let pb = pbo else { dropped += 1; return true }
        CVPixelBufferLockBaseAddress(pb, [])
        if let base = CVPixelBufferGetBaseAddress(pb) {
            let dst = base.assumingMemoryBound(to: UInt8.self)
            let ds = CVPixelBufferGetBytesPerRow(pb)
            let cw = min(w, self.w), ch = min(h, self.h)
            for y in 0..<ch {
                let s = px + y * stride, d = dst + y * ds
                if bgra {
                    d.update(from: s, count: cw * 4)
                } else { // RGBA → BGRA
                    for x in 0..<cw { d[4 * x] = s[4 * x + 2]; d[4 * x + 1] = s[4 * x + 1]; d[4 * x + 2] = s[4 * x]; d[4 * x + 3] = 255 }
                }
            }
        }
        CVPixelBufferUnlockBaseAddress(pb, [])
        if !adaptor.append(pb, withPresentationTime: CMTime(value: ptsUs, timescale: 1_000_000)) { dropped += 1 }
        return writer.status != .failed
    }

    // 音（16 ビットの交互）。時刻はコマと同じ µs
    private func audio(_ pcm: UnsafePointer<Int16>, frames: Int, rate: Int, ch: Int, ptsUs: Int64) -> Bool {
        guard let ain, let afmt else { return true }
        if writer.status == .failed { return false }
        var pts = CMTime(value: ptsUs * Int64(rate) / 1_000_000, timescale: CMTimeScale(rate))
        if let n = aNext, abs(pts.value - n.value) < Int64(rate / 50) { pts = n } // 20 ms より小さいずれは詰める
        aNext = CMTime(value: pts.value + Int64(frames), timescale: CMTimeScale(rate))
        guard ain.isReadyForMoreMediaData else { return true } // 落とす（音は少し途切れる）
        let n = frames * ch * 2
        var bb: CMBlockBuffer?
        guard CMBlockBufferCreateWithMemoryBlock(allocator: kCFAllocatorDefault, memoryBlock: nil, blockLength: n,
                                                 blockAllocator: kCFAllocatorDefault, customBlockSource: nil, offsetToData: 0,
                                                 dataLength: n, flags: kCMBlockBufferAssureMemoryNowFlag, blockBufferOut: &bb) == noErr,
              let bb, CMBlockBufferReplaceDataBytes(with: pcm, blockBuffer: bb, offsetIntoDestination: 0, dataLength: n) == noErr else { return true }
        var sb: CMSampleBuffer?
        guard CMAudioSampleBufferCreateReadyWithPacketDescriptions(allocator: kCFAllocatorDefault, dataBuffer: bb, formatDescription: afmt,
                                                                   sampleCount: frames, presentationTimeStamp: pts,
                                                                   packetDescriptions: nil, sampleBufferOut: &sb) == noErr, let sb else { return true }
        ain.append(sb)
        return writer.status != .failed
    }

    // 書き終えるまで待つ。頼まれていれば「写真」に入れる（入れ終わるのは待たない）
    private func stop() -> Bool {
        vin.markAsFinished()
        ain?.markAsFinished()
        let sem = DispatchSemaphore(value: 0)
        writer.finishWriting { sem.signal() }
        _ = sem.wait(timeout: .now() + 15)
        let ok = writer.status == .completed
        vkr_note("録画：書き終えた（\(ok ? "成功" : "失敗 \(writer.error?.localizedDescription ?? "")")）・落とした \(dropped) コマ")
        if ok && toPhotos {
            let u = url
            PHPhotoLibrary.requestAuthorization(for: .addOnly) { st in
                guard st == .authorized || st == .limited else { vkr_note("録画：写真に入れる許可が無い"); return }
                PHPhotoLibrary.shared().performChanges({
                    PHAssetCreationRequest.forAsset().addResource(with: .video, fileURL: u, options: nil)
                }) { done, err in vkr_note(done ? "録画：写真に入れた" : "録画：写真に入れられない \(err?.localizedDescription ?? "")") }
            }
        }
        return ok
    }

    // 受け手に 4 つの関数を登録する。vkr_serve より前に呼ぶ（つないだときに caps で名乗るため）
    static func register() {
        vkr_set_recorder(
            { name, w, h, fps, codec, kbps, arate, ach, akbps, dest, path, cap in
                let nm = name.map { String(cString: $0) } ?? "iphone-vk"
                guard let r = GameRecorder(name: nm, w: Int(w), h: Int(h), fps: Int(fps), hevc: codec == 1, kbps: Int(kbps),
                                           arate: Int(arate), ach: Int(ach), akbps: Int(akbps), toPhotos: dest & 2 != 0) else { return nil }
                if let path, cap > 0 { // 書く場所を受け手に返す（VR_RECORD_FETCH が読む）
                    let p = Array(r.url.path.utf8CString.prefix(Int(cap) - 1)) + [0]
                    p.withUnsafeBufferPointer { path.update(from: $0.baseAddress!, count: p.count) }
                }
                return Unmanaged.passRetained(r).toOpaque()
            },
            { ctx, px, w, h, stride, bgra, pts in
                guard let ctx, let px else { return 0 }
                let r = Unmanaged<GameRecorder>.fromOpaque(ctx).takeUnretainedValue()
                return r.video(px, w: Int(w), h: Int(h), stride: Int(stride), bgra: bgra != 0, ptsUs: pts) ? 1 : 0
            },
            { ctx, pcm, frames, rate, ch, pts in
                guard let ctx, let pcm else { return 0 }
                let r = Unmanaged<GameRecorder>.fromOpaque(ctx).takeUnretainedValue()
                return r.audio(pcm, frames: Int(frames), rate: Int(rate), ch: Int(ch), ptsUs: pts) ? 1 : 0
            },
            { ctx in
                guard let ctx else { return 0 }
                let r = Unmanaged<GameRecorder>.fromOpaque(ctx).takeRetainedValue() // ここで放す
                return r.stop() ? 1 : 0
            })
    }
}
