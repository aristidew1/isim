// Runs the Kotlin/Native compiler on Linux for an iOS target: the Xcode it looks for is replaced by isim's SDK and
// LLVM's tools (ld64.lld, llvm-libtool-darwin), set up by isim-kotlin.py.
package dev.isim.konan

import org.jetbrains.kotlin.konan.target.Xcode
import org.jetbrains.kotlin.konan.target.XcodeVersion

private class IsimXcode(private val sdk: String, override val toolchain: String) : Xcode {
    override val version = XcodeVersion.maxTested
    override val additionalTools get() = toolchain
    override val simulatorRuntimes get() = "{\"runtimes\": []}"
    override val iphonesimulatorSdk get() = sdk
    override val iphoneosSdk get() = sdk
    override val macosxSdk get() = unsupported()
    override val appletvosSdk get() = unsupported()
    override val appletvsimulatorSdk get() = unsupported()
    override val watchosSdk get() = unsupported()
    override val watchsimulatorSdk get() = unsupported()
    private fun unsupported(): Nothing = error("isim: only iOS targets are supported")
}

fun main(args: Array<String>) {
    val sdk = System.getenv("ISIM_SDK") ?: error("ISIM_SDK is not set")
    val toolchain = System.getenv("ISIM_KONAN_TOOLCHAIN") ?: error("ISIM_KONAN_TOOLCHAIN is not set")
    Xcode.xcodeOverride = IsimXcode(sdk, toolchain)
    org.jetbrains.kotlin.cli.utilities.main(args)
}
