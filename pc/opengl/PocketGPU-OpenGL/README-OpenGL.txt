PocketGPU OpenGL 追加パック

OpenGL のアプリを iPhone の GPU で動かすための部品です。
zink（Mesa）が OpenGL を Vulkan に直し、PocketGPU のドライバが iPhone に送ります。

【入れ方】
このフォルダの中身を、PocketGPU のキットのフォルダ（iphone-vk.cmd があるフォルダ）に、
そのまま上書きでコピーしてください。

【使い方】
1. いつもどおり PocketGPU をつなぎます（Start-PocketGPU.cmd）。
2. OpenGL のアプリの .exe を、Run-on-iPhone-OpenGL.cmd にドラッグします。
   コマンドプロンプトからは Run-on-iPhone-OpenGL.cmd アプリの.exe でも動きます。

【できること・できないこと】
・OpenGL 2.1 までです。OpenGL 3.0 以上を使うアプリは起動しません。
  3.0 には transform feedback という機能が要りますが、iPhone の MoltenVK にありません。
・「描き方が正しくないことがある」という警告が出ることがありますが、そのまま動きます。
・Program Files の中にあるアプリでは、ファイルを置けないので動きません。
  アプリのフォルダごと D:\ などに写してから使ってください。

【分かっている不具合】
・glReadPixels（画面の読み戻し）が真っ黒になります。
  画面への表示は正しく出ます。受け手に届いた絵が正しいことは確かめました。
  スクリーンショットを撮る機能や、ベンチマークの結果の画像の保存が効かないことがあります。
  原因を調べています。

【仕組み】
Windows の OpenGL のアプリは opengl32.dll を直に使うので、そのままでは Vulkan の
ドライバを通りません。この cmd は、アプリの横に zink の opengl32.dll を一時的に置き、
終わったら片づけます。もともと置いてあった物は .pgpubak という名前で取っておきます。
