// iOS 26 の SpeechAnalyzer に、PC から流れてくる音を流しっぱなしにして、途中の結果（volatile）と確定した結果を、すぐ PC に返す。
// AppleFeatures の 50057 番の頼み {"op":"live_asr",…} で始まり、そのつなぎはそこから「流す形」に変わる（AppleFeatures.next は呼ばない）。
//   頼み：{"op":"live_asr","lang":"ja-JP","rate":16000,"channels":1,"format":"s16le","volatile":true,"fast":true}
//   最初の答え（普通の答えと同じ枠）：{"ok":true,"t":"ready","engine","locale","analyzer_format","asset"} か {"error"}
//   そのあと PC → iPhone：[u32][JSON {"t":"pcm","seq","at"}][u32][s16le の PCM]・{"t":"flush"}・{"t":"ping","pc"}・{"t":"end"}（中身は空の枠）
//            iPhone → PC：[u32][JSON]  partial・final（text・start・end は音の始まりからの秒）・pong・stats・error・done
// 音の時間は「受け取った PCM を隙間なくつないだもの」。PC は無音の間も 0 を送り続けるので、PC のコマ番号と秒がそのまま合う。
// 確かめていないこと（実機で）：fastResults と volatileResults の実際の遅れ・アプリが後ろにいるときに続くか・iPhone 17 Pro で同時に何本まで
#if compiler(>=6.2)
import AVFoundation
import CoreMedia
import Foundation
import Network
import Speech

@available(iOS 26.0, *)
final class LiveASR: @unchecked Sendable {
    static let maxActive = 2
    private static let countLock = NSLock()
    private static var active = 0

    private let c: NWConnection
    private let log: (String) -> Void
    private let sendQueue = DispatchQueue(label: "live-asr-send")
    private let inFormat: AVAudioFormat
    private var outFormat: AVAudioFormat?
    private var converter: AVAudioConverter?
    private var analyzer: SpeechAnalyzer?
    private var builder: AsyncStream<AnalyzerInput>.Continuation?
    private var resultsTask: Task<Void, Never>?
    private let stateLock = NSLock()
    private var frames: Int64 = 0          // 受け取ったコマの数（PC の rate で）
    private var lastEnd: Double = 0        // 届いた結果の終わりのいちばん後ろ
    private var finalizedThrough: Double = 0
    private var lastStats = Date()
    private var ended = false
    private var released = false

    private init(_ c: NWConnection, _ fmt: AVAudioFormat, _ log: @escaping (String) -> Void) {
        self.c = c
        self.inFormat = fmt
        self.log = log
    }

    // AppleFeatures.next から呼ばれる。ここから先、このつなぎはこのクラスが持つ
    static func begin(_ c: NWConnection, _ req: [String: Any], log: @escaping (String) -> Void) {
        countLock.lock()
        if active >= maxActive {
            countLock.unlock()
            reject(c, "流しっぱなしの文字起こしは同時に \(maxActive) 本まで")
            return
        }
        active += 1
        countLock.unlock()
        let rate = (req["rate"] as? NSNumber)?.doubleValue ?? 16000
        let channels = (req["channels"] as? NSNumber)?.intValue ?? 1
        guard (req["format"] as? String ?? "s16le") == "s16le", channels == 1, rate >= 8000, rate <= 48000,
              let fmt = AVAudioFormat(commonFormat: .pcmFormatInt16, sampleRate: rate, channels: 1, interleaved: true) else {
            countLock.lock(); active -= 1; countLock.unlock()
            reject(c, "音は s16le・モノラル・8〜48 kHz だけ")
            return
        }
        LiveASR(c, fmt, log).setUp(req)
    }

    private static func reject(_ c: NWConnection, _ msg: String) {
        let out = (try? JSONSerialization.data(withJSONObject: ["error": msg])) ?? Data("{}".utf8)
        var n = UInt32(out.count).littleEndian
        var frame = Data(bytes: &n, count: 4)
        frame.append(out)
        c.send(content: frame, completion: .contentProcessed { _ in c.cancel() })
    }

    private static func authorize() async -> SFSpeechRecognizerAuthorizationStatus {
        let s = SFSpeechRecognizer.authorizationStatus()
        if s != .notDetermined { return s }
        return await withCheckedContinuation { cont in
            SFSpeechRecognizer.requestAuthorization { cont.resume(returning: $0) }
        }
    }

    private static func describe(_ f: AVAudioFormat) -> String {
        let kind: String
        switch f.commonFormat {
        case .pcmFormatInt16: kind = "Int16"
        case .pcmFormatFloat32: kind = "Float32"
        case .pcmFormatInt32: kind = "Int32"
        case .pcmFormatFloat64: kind = "Float64"
        default: kind = "other"
        }
        return "\(Int(f.sampleRate)) Hz \(f.channelCount)ch \(kind)"
    }

    // ---- 始める ----
    private func setUp(_ req: [String: Any]) {
        let lang = req["lang"] as? String ?? "ja-JP"
        let fast = req["fast"] as? Bool ?? true
        let volatile = req["volatile"] as? Bool ?? true
        Task {
            do {
                // 許可は asr と同じ。初めてのときは iPhone の画面で押してもらう
                guard await Self.authorize() == .authorized else {
                    throw AIError("音声認識が許可されていない。iPhone の設定で「計算の受け手」の音声認識を許可する")
                }
                guard let locale = await SpeechTranscriber.supportedLocale(equivalentTo: Locale(identifier: lang)) else {
                    throw AIError("\(lang) は SpeechTranscriber に無い")
                }
                var reporting: Set<SpeechTranscriber.ReportingOption> = []
                if volatile { reporting.insert(.volatileResults) }
                if fast { reporting.insert(.fastResults) } // 文脈の窓を小さくして早く返す（Apple の資料：少し不正確になる）
                let transcriber = SpeechTranscriber(locale: locale, transcriptionOptions: [], reportingOptions: reporting,
                                                    attributeOptions: [.audioTimeRange])
                var asset = "installed"
                if let r = try await AssetInventory.assetInstallationRequest(supporting: [transcriber]) {
                    asset = "downloaded" // 初めての言語は、ここで言葉のモデルを iPhone が落とす
                    try await r.downloadAndInstall()
                }
                guard let best = await SpeechAnalyzer.bestAvailableAudioFormat(compatibleWith: [transcriber]) else {
                    throw AIError("SpeechAnalyzer の音の形が決まらない")
                }
                let analyzer = SpeechAnalyzer(modules: [transcriber])
                try await analyzer.prepareToAnalyze(in: best) // 温めておく（最初の結果を早く）
                let (stream, builder) = AsyncStream.makeStream(of: AnalyzerInput.self)
                self.outFormat = best
                if best != self.inFormat {
                    guard let conv = AVAudioConverter(from: self.inFormat, to: best) else { throw AIError("音の形を変える道具を作れない") }
                    self.converter = conv
                }
                self.analyzer = analyzer
                self.builder = builder
                self.resultsTask = Task { await self.pump(transcriber) }
                try await analyzer.start(inputSequence: stream)
                self.send(["ok": true, "t": "ready", "engine": "SpeechAnalyzer", "locale": locale.identifier,
                           "analyzer_format": Self.describe(best), "asset": asset, "fast": fast])
                self.log("流しっぱなしの文字起こし：始めた（\(locale.identifier)・\(Self.describe(best))・\(asset)）")
                self.readLoop()
            } catch {
                self.send(["error": "流しっぱなしの文字起こしを始められない：\(error.localizedDescription)"]) { self.close() }
            }
        }
    }

    // ---- 結果を PC へ ----
    private func pump(_ t: SpeechTranscriber) async {
        do {
            for try await r in t.results {
                let start = AIShared.fin(r.range.start.seconds)
                let end = AIShared.fin(r.range.end.seconds)
                var ev: [String: Any] = ["t": r.isFinal ? "final" : "partial", "text": String(r.text.characters),
                                         "start": start, "end": end]
                stateLock.lock()
                lastEnd = max(lastEnd, end)
                if r.isFinal {
                    finalizedThrough = max(finalizedThrough, AIShared.fin(r.resultsFinalizationTime.seconds))
                    ev["finalized_through"] = finalizedThrough
                }
                stateLock.unlock()
                send(ev)
            }
        } catch {
            send(["t": "error", "error": "文字起こしが止まった：\(error.localizedDescription)"])
        }
    }

    private func send(_ obj: [String: Any], then: (() -> Void)? = nil) {
        sendQueue.async {
            guard let out = try? JSONSerialization.data(withJSONObject: obj) else { then?(); return }
            var n = UInt32(out.count).littleEndian
            var frame = Data(bytes: &n, count: 4)
            frame.append(out)
            self.c.send(content: frame, completion: .contentProcessed { _ in then?() })
        }
    }

    private func audioSeconds() -> Double {
        stateLock.lock(); defer { stateLock.unlock() }
        return AIShared.fin(Double(frames) / inFormat.sampleRate)
    }

    private func maybeStats() {
        guard Date().timeIntervalSince(lastStats) >= 1 else { return }
        lastStats = Date()
        stateLock.lock()
        let a = Double(frames) / inFormat.sampleRate
        let e = lastEnd, f = finalizedThrough
        stateLock.unlock()
        // backlog_s：受け取った音と、届いた結果の終わりの差。黙っている間も増えるので、話している間の値を見る
        send(["t": "stats", "audio_s": AIShared.fin(a), "result_end_s": e, "finalized_s": f, "backlog_s": AIShared.fin(a - e)])
    }

    // ---- 受ける ----
    private func readExactly(_ n: Int, _ done: @escaping (Data?) -> Void) {
        if n == 0 { done(Data()); return }
        c.receive(minimumIncompleteLength: n, maximumLength: n) { data, _, _, err in
            if let data, data.count == n, err == nil { done(data) } else { done(nil) }
        }
    }

    private func readFrame(_ done: @escaping (Data?) -> Void) {
        readExactly(4) { [weak self] h in
            guard let self, let h else { done(nil); return }
            let n = Int(h.withUnsafeBytes { $0.loadUnaligned(as: UInt32.self) }.littleEndian)
            if n > 8 << 20 { done(nil); return } // 1 塊の PCM は小さい。大きすぎる枠は切る
            self.readExactly(n, done)
        }
    }

    private func readLoop() {
        readFrame { [self] head in
            guard let head else { self.finish(abort: true); return }
            self.readFrame { body in
                guard let body else { self.finish(abort: true); return }
                let m = (try? JSONSerialization.jsonObject(with: head)) as? [String: Any] ?? [:]
                switch m["t"] as? String ?? "" {
                case "pcm":
                    self.feed(body)
                case "ping":
                    self.send(["t": "pong", "pc": m["pc"] ?? NSNull(), "audio_s": self.audioSeconds()])
                case "flush":
                    Task { try? await self.analyzer?.finalize(through: nil) }
                case "end":
                    self.finish(abort: false)
                    return
                default:
                    break
                }
                self.maybeStats()
                self.readLoop()
            }
        }
    }

    private func feed(_ data: Data) {
        let n = data.count / 2
        guard n > 0, let builder, let buf = AVAudioPCMBuffer(pcmFormat: inFormat, frameCapacity: AVAudioFrameCount(n)) else { return }
        buf.frameLength = AVAudioFrameCount(n)
        data.withUnsafeBytes { raw in
            if let dst = buf.int16ChannelData?[0], let src = raw.baseAddress { memcpy(dst, src, n * 2) }
        }
        stateLock.lock(); frames += Int64(n); stateLock.unlock()
        guard let converter, let outFormat else {
            builder.yield(AnalyzerInput(buffer: buf))
            return
        }
        let cap = AVAudioFrameCount(Double(n) * outFormat.sampleRate / inFormat.sampleRate) + 64
        guard let out = AVAudioPCMBuffer(pcmFormat: outFormat, frameCapacity: cap) else { return }
        var given = false
        var err: NSError?
        // noDataNow：この塊は渡し終えたが、流れは続く（endOfStream にしない）。rate を変えるときの続きの状態を道具が持つ
        let st = converter.convert(to: out, error: &err) { _, status in
            if given {
                status.pointee = .noDataNow
                return nil
            }
            given = true
            status.pointee = .haveData
            return buf
        }
        if st == .error {
            log("流しっぱなしの文字起こし：音の形を変えられない \(err?.localizedDescription ?? "")")
            return
        }
        if out.frameLength > 0 { builder.yield(AnalyzerInput(buffer: out)) }
    }

    // ---- 終わる ----
    private func finish(abort: Bool) {
        stateLock.lock()
        if ended { stateLock.unlock(); return }
        ended = true
        stateLock.unlock()
        builder?.finish()
        Task {
            if abort {
                await self.analyzer?.cancelAndFinishNow()
            } else {
                try? await self.analyzer?.finalizeAndFinishThroughEndOfInput() // 残りを確定させてから
            }
            _ = await self.resultsTask?.value
            if abort {
                self.close()
            } else {
                self.send(["t": "done", "audio_s": self.audioSeconds()]) { self.close() }
            }
            self.log("流しっぱなしの文字起こし：終わった（\(Int(self.audioSeconds())) 秒）")
        }
    }

    private func close() {
        stateLock.lock()
        let first = !released
        released = true
        stateLock.unlock()
        guard first else { return }
        c.cancel()
        Self.countLock.lock(); Self.active -= 1; Self.countLock.unlock()
    }
}
#endif
