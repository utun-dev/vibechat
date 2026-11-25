package com.google.mlkit.vision.barcode

import com.google.android.gms.tasks.Task
import com.google.mlkit.vision.barcode.common.Barcode
import com.google.mlkit.vision.common.InputImage

class BarcodeScannerOptions {
    class Builder {
        fun setBarcodeFormats(format: Int) = this
        fun build() = BarcodeScannerOptions()
    }
}
class BarcodeScanner {
    var failures = emptySet<String>()
    val tasks = mutableListOf<Task<List<Barcode>>>()
    var closed = false
    var closes = 0
    fun process(image: InputImage): Task<List<Barcode>> {
        check(!closed && "process" !in failures)
        return Task<List<Barcode>>().also { it.failures = failures; tasks.add(it) }
    }
    fun close() {
        closes++; check("close" !in failures) { "Scanner close failed" }; closed = true
        check("close after" !in failures) { "Scanner close failed after effect" }
    }
}
object BarcodeScanning {
    var failCreate = false
    val created = mutableListOf<BarcodeScanner>()
    fun getClient(options: BarcodeScannerOptions): BarcodeScanner {
        check(!failCreate) { "Scanner creation failed" }
        return BarcodeScanner().also { created.add(it) }
    }
}
