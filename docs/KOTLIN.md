# Kotlin Multiplatform apps

An iOS app whose logic is a Kotlin Multiplatform module links a framework the Kotlin/Native compiler produces. On a
Mac, Gradle builds it (`embedAndSignAppleFrameworkForXcode`); JetBrains' compiler for Linux compiles Kotlin to iOS
klibs but refuses to link them, because the link step wants Xcode. `isim kotlin` does that link on Linux with isim's
SDK and LLVM's tools, so the whole app (Kotlin logic and SwiftUI interface) builds and runs in isim.

```bash
# 1. Gradle compiles the klibs for the simulator's x86_64 target (works on Linux)
./gradlew compileKotlinIosX64

# 2. isim links them into a static framework
isim kotlin framework -name Shared -include build/classes/kotlin/iosX64/main/klib/shared \
    -library ~/.gradle/…/kotlinx-coroutines-core-iosX64Main-1.11.0.klib … -o build/isim-framework -g

# 3. the app links it like any framework
isim build -project App.xcodeproj -scheme App -skip-script-phases FRAMEWORK_SEARCH_PATHS=$PWD/build/isim-framework
```

`-klibs FILE` reads the klib paths from a file (one per line, the first is the one whose API the framework exports), so
a small Gradle task can write them:

```kotlin
val compilation = kotlin.iosX64().compilations.getByName("main")
val own = compilation.compileTaskProvider.map { (it as org.jetbrains.kotlin.gradle.tasks.KotlinNativeCompile).outputFile.get() }
val libraries: FileCollection = compilation.compileDependencyFiles
tasks.register("isimKlibs") {
    dependsOn(compilation.compileTaskProvider)
    val output = layout.buildDirectory.file("isim-klibs.txt")
    inputs.files(libraries); outputs.file(output)
    doLast { output.get().asFile.writeText((listOf(own.get()) + libraries.files).joinToString("\n") { it.absolutePath } + "\n") }
}
```

Arguments after `--` go to the Kotlin/Native compiler (`-Xbinary=…`, `-Xexport-library=…`). `-skip-script-phases`
skips the Xcode project's Gradle script phase, which only works on a Mac.

## What it does

The first use of a Kotlin version (read from the klib; `isim kotlin install VERSION` does it ahead) sets up
`~/.local/share/isim-kotlin/VERSION` (`ISIM_KOTLIN_HOME`):

- JetBrains' Kotlin/Native distribution for Linux, which already holds the iOS platform libraries;
- the Kotlin runtime for the iOS targets: LLVM bitcode, the same on every host, which only the macOS distribution
  ships (`konan/targets/ios_*`);
- `HostManager.kt` from that Kotlin release with one line changed (the iOS targets enabled on a Linux host), and
  `tools/IsimKonan.kt`, which starts the compiler with isim's SDK and LLVM's `ld64.lld` / `llvm-libtool-darwin` where
  it expects Xcode (the compiler's own `Xcode.xcodeOverride`).

The framework is the compiler's normal output (headers, module map, static library). isim adds compiler-rt's
`__cpu_model` to it, which clang links by itself on a Mac.

Needs Java 17 or newer and LLVM 21 or newer on `PATH`, like `isim cc`. No Apple software is used.

## Limits

- The simulator target is `iosX64` (isim runs x86_64 simulator binaries). Kotlin has deprecated it; while your Kotlin
  version still has it, declare it next to `iosSimulatorArm64` (only for isim, e.g. behind a Gradle property).
- Static frameworks only (`isStatic = true`, the usual choice).
- cinterop of your own C or Objective-C headers is compiled by Gradle, not by isim; klibs that only use the platform
  libraries (Foundation, UIKit, Security…) need nothing more.
- The Kotlin release must still have the line isim changes in `HostManager.kt`; isim says so when it does not.
