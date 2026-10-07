# OpenGL を iPhone で：zink の道（2026-10-07 調べ）

Windows の OpenGL のアプリを、Mesa の zink（OpenGL を Vulkan に直す部品）に通し、このドライバ越しに iPhone で描く。

## zink（Mesa 26.2）が起動に必須とする物と、このドライバ
| 必須 | iPhone（MoltenVK 1.4.2） | このドライバ |
|---|---|---|
| VK_KHR_maintenance1 | ある | 名乗る |
| VK_KHR_maintenance5（2026-05 から必須） | ある | IPHONE_VK_DXVK3=1 で名乗る |
| VK_KHR_create_renderpass2 | ある | 受け手が名乗れば名乗る |
| VK_KHR_dynamic_rendering | ある | 受け手が名乗れば名乗る |
| VK_KHR_descriptor_update_template | ある | 名乗る（ドライバの中でふつうの書き込みに直す・10/7） |
| timeline semaphore | ある | 名乗る |
| robustness2 の nullDescriptor（2025-06 から必須） | **無い** | IPHONE_VK_DXVK3=1 で名乗る。空の descriptor は受け手が代わりの物に差し替える |

最後の nullDescriptor が、ふつうの MoltenVK の上で zink が動かない理由（Mesa の issue #15649）。このドライバは受け手の差し替えで肩代わりできる見込み。

## 見込み
- 出るのは OpenGL 2.1（と ES 2.0）まで。3.0 には transform feedback が要り、MoltenVK に無い
- 警告だけで続く物：logicOp・custom border color・line rasterization が無いと「描き方が正しくないことがある」と出る（ZINK_DEBUG=quiet で消える）
- 試し方の案：mesa-dist-win 26.2.4 の opengl32.dll と libgallium_wgl.dll をアプリの横に置き、GALLIUM_DRIVER=zink・IPHONE_VK_DXVK3=1・IPHONE_VK_ONLY=1 で iphone-vk.cmd から起動する。配布物は .7z（Windows 11 の tar で開ける見込み・未確認）

## まだ
- mesa-dist-win を取ってくる（54〜67 MB・取ってくるのは CEO に聞いてから）
- 2.1 で動く OpenGL のアプリを選ぶ

出典：Mesa 26.2 の src/gallium/drivers/zink（zink_screen.c・zink_device_info.py）、MR !37115・!44047、issue #15649、MoltenVK v1.4.2 の Docs/MoltenVK_Runtime_UserGuide.md と MVKDevice.mm（調べは Sonnet の助手。ソースの行は助手の読み）
