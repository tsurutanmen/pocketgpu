// exe の情報。TargetFramework を書くと、WPF の互換の切り替えが 4.8 の既定になる（モニターごとの DPI で描き直すなど）
using System.Reflection;
using System.Runtime.Versioning;

[assembly: TargetFramework(".NETFramework,Version=v4.8", FrameworkDisplayName = ".NET Framework 4.8")]
[assembly: AssemblyTitle("PocketGPU")]
[assembly: AssemblyProduct("PocketGPU")]
[assembly: AssemblyDescription("PocketGPU Windows app")]
[assembly: AssemblyVersion("0.1.0.0")]
[assembly: AssemblyFileVersion("0.1.0.0")]
