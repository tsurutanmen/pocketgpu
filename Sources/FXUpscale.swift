import Foundation
import Metal
import MetalFX

// PC のドライバが頼んだとき（VR_H264_UP15）、窓に出す絵を H.264 に縮める前に MetalFX の空間の拡大で 1.5 倍に広げる。
// 720p で描いた絵を 1080p にして PC に返す。受け手（vk/remote/server.c）に関数を登録し、受け手の糸から呼ばれる。
// 入れる絵は CPU から見える BGRA（RGBA でも並びのまま扱うので色は崩れない）。出す絵も同じ並び
final class FXUpscaler {
    static let shared = FXUpscaler()

    private let lock = NSLock()
    private let device = MTLCreateSystemDefaultDevice()
    private lazy var queue: MTLCommandQueue? = device?.makeCommandQueue()
    private var scaler: MTLFXSpatialScaler?
    private var inTex: MTLTexture?
    private var outTex: MTLTexture?
    private var outBuf: MTLBuffer?
    private var sizes = [0, 0, 0, 0]

    // GPU が MetalFX の空間の拡大を使えるときだけ登録する。vkr_serve より前に呼ぶ（つないだときに名乗るため）
    static func register() {
        guard let d = MTLCreateSystemDefaultDevice(), MTLFXSpatialScalerDescriptor.supportsDevice(d) else { return }
        vkr_set_upscaler { src, w, h, stride, dst, dw, dh, dstride in
            guard let src = src, let dst = dst else { return 0 }
            return FXUpscaler.shared.run(src: src, w: Int(w), h: Int(h), stride: Int(stride),
                                         dst: dst, dw: Int(dw), dh: Int(dh), dstride: Int(dstride)) ? 1 : 0
        }
    }

    // 大きさが変わったときだけ、拡大の部品と絵の入れ物を作り直す
    private func prepare(w: Int, h: Int, dw: Int, dh: Int) -> Bool {
        if sizes == [w, h, dw, dh], scaler != nil { return true }
        guard let dev = device else { return false }
        let desc = MTLFXSpatialScalerDescriptor()
        desc.inputWidth = w
        desc.inputHeight = h
        desc.outputWidth = dw
        desc.outputHeight = dh
        desc.colorTextureFormat = .bgra8Unorm
        desc.outputTextureFormat = .bgra8Unorm
        desc.colorProcessingMode = .perceptual
        guard let sc = desc.makeSpatialScaler(device: dev) else { return false }
        let ti = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .bgra8Unorm, width: w, height: h, mipmapped: false)
        ti.usage = sc.colorTextureUsage
        ti.storageMode = .shared
        let to = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .bgra8Unorm, width: dw, height: dh, mipmapped: false)
        to.usage = sc.outputTextureUsage
        to.storageMode = .private
        guard let a = dev.makeTexture(descriptor: ti), let b = dev.makeTexture(descriptor: to),
              let buf = dev.makeBuffer(length: dw * dh * 4, options: .storageModeShared) else { return false }
        scaler = sc
        inTex = a
        outTex = b
        outBuf = buf
        sizes = [w, h, dw, dh]
        return true
    }

    func run(src: UnsafePointer<UInt8>, w: Int, h: Int, stride: Int,
             dst: UnsafeMutablePointer<UInt8>, dw: Int, dh: Int, dstride: Int) -> Bool {
        lock.lock()
        defer { lock.unlock() }
        guard prepare(w: w, h: h, dw: dw, dh: dh), let sc = scaler, let a = inTex, let b = outTex, let buf = outBuf,
              let q = queue, let cb = q.makeCommandBuffer() else { return false }
        a.replace(region: MTLRegionMake2D(0, 0, w, h), mipmapLevel: 0, withBytes: UnsafeRawPointer(src), bytesPerRow: stride)
        sc.colorTexture = a
        sc.outputTexture = b
        sc.inputContentWidth = w
        sc.inputContentHeight = h
        sc.encode(commandBuffer: cb)
        guard let blit = cb.makeBlitCommandEncoder() else { return false }
        blit.copy(from: b, sourceSlice: 0, sourceLevel: 0, sourceOrigin: MTLOrigin(x: 0, y: 0, z: 0),
                  sourceSize: MTLSize(width: dw, height: dh, depth: 1),
                  to: buf, destinationOffset: 0, destinationBytesPerRow: dw * 4, destinationBytesPerImage: dw * dh * 4)
        blit.endEncoding()
        cb.commit()
        cb.waitUntilCompleted()
        if cb.status != .completed { return false }
        let p = UnsafeRawPointer(buf.contents())
        let row = dw * 4
        if dstride == row {
            memcpy(UnsafeMutableRawPointer(dst), p, row * dh)
        } else {
            for y in 0..<dh {
                memcpy(UnsafeMutableRawPointer(dst + y * dstride), p + y * row, row)
            }
        }
        return true
    }
}
