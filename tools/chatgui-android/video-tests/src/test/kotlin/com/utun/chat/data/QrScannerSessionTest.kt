package com.utun.chat.data

import android.content.Context
import android.os.Handler
import android.os.Looper
import androidx.camera.core.ImageAnalysis
import androidx.camera.core.ImageProxy
import androidx.camera.core.Preview
import androidx.camera.lifecycle.HostCameraFuture
import androidx.camera.lifecycle.ProcessCameraProvider
import androidx.camera.view.PreviewView
import androidx.lifecycle.LifecycleOwner
import com.google.mlkit.vision.barcode.BarcodeScanning
import com.google.mlkit.vision.barcode.common.Barcode
import org.junit.After
import org.junit.Assert.*
import org.junit.Before
import org.junit.Test

/** Настоящий владелец CameraX/ML Kit; fault injection только на границах SDK и очереди main. */
class QrScannerSessionTest {
    private val context = Context()
    private val lifecycle = object : LifecycleOwner {}
    private val sessions = mutableListOf<QrScannerSession>()
    private val images = mutableListOf<ImageProxy>()
    private val errors = mutableListOf<QrScannerSession.Failure>()
    private val main get() = Looper.getMainLooper()
    private val scanner get() = BarcodeScanning.created.single()
    private val analysis get() = ImageAnalysis.created.single()
    private val provider get() = ProcessCameraProvider.shared

    @Before fun prepare() {
        Handler.rejectImmediate = false; Handler.rejectDelayed = false
        BarcodeScanning.failCreate = false; BarcodeScanning.created.clear()
        ImageAnalysis.created.clear(); ImageAnalysis.nextFailures = emptySet()
        ProcessCameraProvider.shared = ProcessCameraProvider()
        ProcessCameraProvider.future = HostCameraFuture().apply { ready = true }
        ProcessCameraProvider.failLookup = false; LogManager.entries.clear()
    }
    @After fun release() {
        Handler.rejectImmediate = false; Handler.rejectDelayed = false
        provider.failures = emptySet(); ImageAnalysis.created.forEach { it.failures = emptySet() }
        images.forEach { it.failures = emptySet() }
        sessions.forEach { it.close() }
        BarcodeScanning.created.forEach { sdk ->
            sdk.failures = emptySet()
            sdk.tasks.filter { !it.isComplete }.forEach { it.finish(emptyList()) }
        }
        main.advanceBy(30_000); sessions.forEach { it.close() }; main.advanceBy(30_000)
        assertTrue(images.all { it.closed })
        assertTrue(BarcodeScanning.created.all { it.closed })
    }
    private fun session(onScan: (List<String>) -> Boolean = { false }): QrScannerSession =
        QrScannerSession(context, lifecycle, PreviewView(context), onScan, errors::add).also { sessions.add(it) }
    private fun start(onScan: (List<String>) -> Boolean = { false }): QrScannerSession = session(onScan).also { it.start(); main.advanceBy(0) }
    private fun frame(): ImageProxy = ImageProxy().also { images.add(it); analysis.analyzer!!(it) }
    private fun diagnostic(fragment: String) = assertTrue("Missing diagnostic: $fragment", LogManager.entries.any {
        it.level == "ERROR" && fragment in it.message
    })

    @Test fun constructionAndCloseBeforeStartDoNotCreateSdkResources() {
        val owner = session(); owner.close(); owner.close()
        assertTrue(BarcodeScanning.created.isEmpty()); assertTrue(ImageAnalysis.created.isEmpty())
        assertEquals(0, provider.binds)
    }
    @Test fun closeUnbindsOnlyItsOwnUseCasesAndIsIdempotent() {
        val foreign = Preview(); provider.bound.add(foreign)
        val owner = start(); assertEquals(3, provider.bound.size)
        owner.close(); owner.close()
        assertEquals(setOf(foreign), provider.bound); assertEquals(1, provider.unbinds); assertEquals(1, scanner.closes)
    }
    @Test fun disposeKeepsImageAndScannerUntilRecognitionCompletesOffMain() {
        var deliveries = 0
        val owner = start { deliveries++; true }; val image = frame()
        owner.close(); assertFalse(image.closed); assertFalse(scanner.closed); assertTrue(provider.bound.isEmpty())
        val completion = Thread { scanner.tasks.single().finish(listOf(Barcode("utun://valid"))) }
        completion.start(); completion.join(1000); assertFalse(completion.isAlive)
        assertTrue(image.closed); assertFalse(scanner.closed)
        main.advanceBy(0); assertTrue(scanner.closed); assertEquals(0, deliveries)
    }
    @Test fun lateProviderCompletionCannotBindAfterDispose() {
        ProcessCameraProvider.future.ready = false
        val owner = start(); owner.close(); ProcessCameraProvider.future.complete(); main.advanceBy(0)
        assertEquals(0, provider.binds); assertTrue(scanner.closed)
    }
    @Test fun partialBindFailureStillUnbindsAndReleasesScanner() {
        provider.failures = setOf("bind")
        start()
        assertEquals(listOf(QrScannerSession.Failure.CAMERA), errors)
        assertTrue(provider.bound.isEmpty()); assertTrue(scanner.closed); diagnostic("Camera binding failed")
    }
    @Test fun scannerConstructionFailureStillLeavesDisposableSession() {
        BarcodeScanning.failCreate = true
        val owner = start(); owner.close()
        assertEquals(listOf(QrScannerSession.Failure.CAMERA), errors); assertTrue(BarcodeScanning.created.isEmpty())
        diagnostic("Camera initialization failed")
    }
    @Test fun partialAnalyzerInstallationIsRemovedOnStartupFailure() {
        ImageAnalysis.nextFailures = setOf("set analyzer")
        start()
        assertNull(analysis.analyzer); assertTrue(scanner.closed); diagnostic("Camera initialization failed")
    }
    @Test fun failedAnalyzerRemovalDoesNotSkipCameraOrScannerCleanup() {
        val owner = start(); analysis.failures = setOf("clear"); owner.close()
        assertTrue(provider.bound.isEmpty()); assertTrue(scanner.closed); assertNotNull(analysis.analyzer)
        analysis.failures = emptySet(); main.advanceBy(250)
        assertNull(analysis.analyzer); assertEquals(2, analysis.clears); diagnostic("Analyzer removal failed")
    }
    @Test fun unbindFailureRetainsUseCasesForAutomaticRetry() {
        val owner = start(); provider.failures = setOf("unbind"); owner.close()
        assertEquals(2, provider.bound.size); assertTrue(scanner.closed)
        provider.failures = emptySet(); main.advanceBy(250)
        assertTrue(provider.bound.isEmpty()); assertEquals(2, provider.unbinds); diagnostic("Camera unbind failed")
    }
    @Test fun scannerCloseFailuresBeforeAndAfterEffectAreRetried() {
        val owner = start(); scanner.failures = setOf("close"); owner.close()
        assertFalse(scanner.closed)
        scanner.failures = setOf("close after"); main.advanceBy(250); assertTrue(scanner.closed)
        scanner.failures = emptySet(); main.advanceBy(500); assertEquals(3, scanner.closes)
        owner.close(); assertEquals(3, scanner.closes); diagnostic("Scanner close failed")
    }
    @Test fun imageCloseFailureCannotReleaseScannerAndIsRetriedAfterTaskCompletion() {
        val owner = start(); val image = frame(); image.failures = setOf("close")
        owner.close(); scanner.tasks.single().finish(emptyList()); main.advanceBy(0)
        assertFalse(image.closed); assertFalse(scanner.closed)
        image.failures = emptySet(); main.advanceBy(250)
        assertTrue(image.closed); assertTrue(scanner.closed); diagnostic("Image close failed")
    }
    @Test fun listenerRegistrationFailureCannotCloseImageUnderRunningTask() {
        start(); scanner.failures = setOf("listener"); val image = frame(); main.advanceBy(0)
        assertFalse(image.closed); assertFalse(scanner.closed); assertEquals(listOf(QrScannerSession.Failure.SCAN), errors)
        scanner.tasks.single().finish(emptyList()); assertFalse(image.closed)
        main.advanceBy(250); assertTrue(image.closed); assertTrue(scanner.closed); diagnostic("Image submission/listener failed")
    }
    @Test fun listenerFailureAfterRegistrationStillClosesImageOnlyOnce() {
        start(); scanner.failures = setOf("listener after"); val image = frame(); main.advanceBy(0)
        assertFalse(image.closed)
        scanner.tasks.single().finish(emptyList()); main.advanceBy(0)
        assertEquals(1, image.closes); assertTrue(scanner.closed)
    }
    @Test fun processExceptionWithoutTaskClosesImageAndReportsFailure() {
        start(); scanner.failures = setOf("process"); val image = frame(); main.advanceBy(0)
        assertTrue(image.closed); assertTrue(scanner.closed); assertEquals(listOf(QrScannerSession.Failure.SCAN), errors)
    }
    @Test fun invalidResultContinuesButAcceptedResultClosesOnce() {
        val results = mutableListOf<List<String>>()
        start { results.add(it); it.any { text -> text == "utun://valid" } }
        frame(); scanner.tasks[0].finish(listOf(Barcode("other"))); main.advanceBy(0)
        assertFalse(scanner.closed)
        frame(); scanner.tasks[1].finish(listOf(Barcode("utun://valid"))); main.advanceBy(0)
        assertTrue(scanner.closed); assertEquals(2, results.size); assertTrue(errors.isEmpty())
    }
    @Test fun resultCallbackFailureReleasesSdkAndReportsError() {
        start { error("UI result failed") }; frame(); scanner.tasks.single().finish(emptyList()); main.advanceBy(0)
        assertTrue(scanner.closed); assertEquals(listOf(QrScannerSession.Failure.SCAN), errors)
        diagnostic("result callback failed")
    }
    @Test fun taskFailureAndCancellationCannotStrandImages() {
        start(); val image = frame(); scanner.tasks.single().finish(failure = IllegalStateException("recognition failure"))
        main.advanceBy(0)
        assertTrue(image.closed); assertTrue(scanner.closed); diagnostic("recognition/result callback failed")
    }
    @Test fun canceledTaskClosesImageAndReportsFailure() {
        start(); val image = frame(); scanner.tasks.single().finish(canceled = true); main.advanceBy(0)
        assertTrue(image.closed); assertTrue(scanner.closed); assertEquals(listOf(QrScannerSession.Failure.SCAN), errors)
    }
    @Test fun lateAnalyzerCallbackAfterDisposeOnlyClosesItsImage() {
        val owner = start(); val late = analysis.analyzer!!; owner.close()
        val image = ImageProxy().also { images.add(it) }; late(image)
        assertTrue(image.closed); assertTrue(scanner.tasks.isEmpty()); assertTrue(scanner.closed)
    }
    @Test fun rejectedMainCompletionStillSchedulesIndependentSdkCleanup() {
        start(); val image = frame(); Handler.rejectImmediate = true
        scanner.tasks.single().finish(emptyList()); assertTrue(image.closed); assertFalse(scanner.closed)
        Handler.rejectImmediate = false; main.advanceBy(250)
        assertTrue(scanner.closed); assertTrue(provider.bound.isEmpty()); diagnostic("Main callback rejected")
    }
    @Test fun rejectedCleanupTimerPreservesOwnerForExplicitRetry() {
        val owner = start(); scanner.failures = setOf("close"); Handler.rejectDelayed = true; owner.close()
        assertFalse(scanner.closed); diagnostic("Cleanup retry rejected")
        scanner.failures = emptySet(); Handler.rejectDelayed = false; owner.close()
        assertTrue(scanner.closed)
    }
}
