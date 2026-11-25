package com.utun.chat.data

import android.content.Context
import android.os.Looper
import androidx.appcompat.app.AppCompatDelegate
import androidx.core.content.ContextCompat
import androidx.core.os.LocaleListCompat
import com.utun.chat.ChatApplication
import java.util.Locale
import java.util.concurrent.atomic.AtomicReference
import org.junit.Assert.*
import org.junit.Before
import org.junit.Test

/** Настоящий AppLanguage: каталог XML, requested/effective locale, ошибки Android и отсутствие оптимистического выбора. */
class AppLanguageTest {
    private lateinit var context: Context
    private fun xml(body: String) = """<locale-config xmlns:android="http://schemas.android.com/apk/res/android">$body</locale-config>"""
    private fun error(fragment: String) = assertTrue("missing error $fragment: ${LogManager.entries}",
        LogManager.entries.any { it.level == "ERROR" && fragment in it.message })

    @Before fun prepare() {
        assertSame(Looper.getMainLooper(), Looper.myLooper())
        context = Context(); ChatApplication.instance = context
        ContextCompat.failContext = false
        AppCompatDelegate.requested = LocaleListCompat.forLanguageTags("")
        AppCompatDelegate.failRead = false; AppCompatDelegate.failWrite = false; AppCompatDelegate.selections.clear()
        AppCompatDelegate.failWriteAfter = false
        HostLanguageNotifications.attempts.clear(); HostLanguageNotifications.failures.clear()
        LogManager.entries.clear()
    }

    @Test fun catalogUsesTheRealProjectXmlAndClosesItsParser() {
        assertTrue(AppLanguage.readSupportedTags().containsAll(listOf("en", "ru")))
        assertEquals(1, context.resources.opened); assertEquals(1, context.resources.closed)
    }

    @Test fun catalogNormalizesBcp47AndPreservesDeclarationOrder() {
        context.resources.xml = xml("""<locale android:name="RU"/><locale android:name="en-us"/>""")
        assertEquals(listOf("ru", "en-US"), AppLanguage.readSupportedTags())
        assertEquals(context.resources.opened, context.resources.closed)
    }

    @Test fun invalidCatalogEntriesAreRejectedAtomicallyAndClosed() {
        val bodies = listOf("", "<locale/>", """<locale android:name=""/>""", """<locale android:name=" "/>""",
            """<locale android:name="en_XX"/>""", """<locale android:name="und"/>""",
            """<locale android:name="en"/><locale android:name="EN"/>""")
        for (body in bodies) {
            context.resources.xml = xml(body)
            assertThrows(Exception::class.java) { AppLanguage.readSupportedTags() }
            assertEquals(context.resources.opened, context.resources.closed)
        }
        error("catalog load failed")
    }

    @Test fun parserAndOpenFailuresAreReportedAndAllowAReadRetry() {
        context.resources.failOpen = true
        assertThrows(IllegalStateException::class.java) { AppLanguage.readSupportedTags() }
        error("locale resource unavailable")
        context.resources.failOpen = false; context.resources.failNext = true
        assertThrows(IllegalStateException::class.java) { AppLanguage.readSupportedTags() }
        assertEquals(context.resources.opened, context.resources.closed)
        context.resources.failNext = false
        assertTrue(AppLanguage.readSupportedTags().containsAll(listOf("en", "ru")))
        assertEquals(context.resources.opened, context.resources.closed)
    }

    @Test fun successfulCatalogIsCachedIndependentlyOfConfigurationSignals() {
        val catalog = AppLanguage.supportedTags
        context.resources.failOpen = true
        assertSame(catalog, AppLanguage.supportedTags)
        assertTrue(catalog.containsAll(listOf("en", "ru")))
    }

    @Test fun languageSelectionAndSystemResetUseAppCompatWithoutRefreshingServicesDirectly() {
        assertTrue(AppLanguage.select("ru")); assertEquals("ru", AppLanguage.selectedTag())
        assertTrue(AppLanguage.select("ru")); assertEquals(listOf("ru"), AppCompatDelegate.selections)
        assertTrue(AppLanguage.select("")); assertEquals("", AppLanguage.selectedTag())
        assertEquals(listOf("ru", ""), AppCompatDelegate.selections)
        assertTrue(HostLanguageNotifications.attempts.isEmpty())
    }

    @Test fun unsupportedTagCannotChangePersistedSelection() {
        AppCompatDelegate.requested = LocaleListCompat.forLanguageTags("ru")
        assertFalse(AppLanguage.select("not_declared")); assertFalse(AppLanguage.select("ru,en"))
        assertEquals("ru", AppLanguage.selectedTag()); assertTrue(AppCompatDelegate.selections.isEmpty())
        error("Unsupported language")
    }

    @Test fun failedFrameworkWriteBeforeEffectPreservesSelectionAndIsReportedBeforeSuccessLogging() {
        AppCompatDelegate.requested = LocaleListCompat.forLanguageTags("en")
        AppCompatDelegate.failWrite = true
        assertFalse(AppLanguage.select("ru"))
        assertEquals("en", AppLanguage.selectedTag()); assertTrue(AppCompatDelegate.selections.isEmpty())
        error("locales write failed")
        assertTrue(LogManager.entries.none { it.level == "INFO" })
        AppCompatDelegate.failWrite = false
        assertTrue(AppLanguage.select("ru")); assertEquals("ru", AppLanguage.selectedTag())
    }

    @Test fun partialFrameworkFailureIsReportedWithoutInventingARollback() {
        AppCompatDelegate.requested = LocaleListCompat.forLanguageTags("en")
        AppCompatDelegate.failWriteAfter = true
        assertFalse(AppLanguage.select("ru"))
        assertEquals("ru", AppLanguage.selectedTag())
        assertEquals(listOf("ru"), AppCompatDelegate.selections)
        error("after effect")
        assertTrue(LogManager.entries.none { it.level == "INFO" })
    }

    @Test fun failedSelectionReadIsNotReplacedWithFollowSystem() {
        AppCompatDelegate.failRead = true
        assertThrows(IllegalStateException::class.java) { AppLanguage.selectedTag() }
        assertFalse(AppLanguage.select("ru")); assertTrue(AppCompatDelegate.selections.isEmpty())
        error("selection read failed")
    }

    @Test fun requestedLocaleListAndItsLabelKeepEveryPriority() {
        AppCompatDelegate.requested = LocaleListCompat.forLanguageTags("ru,en")
        assertEquals("ru,en", AppLanguage.selectedTag())
        assertEquals("Русский, English", AppLanguage.nativeName(AppLanguage.selectedTag()))
    }

    @Test fun displayNamesUseTheirOwnLanguageAndHaveANonblankFallback() {
        val previous = Locale.getDefault()
        try {
            Locale.setDefault(Locale.forLanguageTag("tr"))
            assertEquals("English", AppLanguage.nativeName("en"))
            assertEquals("Русский", AppLanguage.nativeName("ru"))
            assertEquals("!", AppLanguage.nativeName("!"))
            assertEquals("", AppLanguage.nativeName(""))
        } finally { Locale.setDefault(previous) }
    }

    @Test fun textUsesEffectiveContextAndPassesFormattingArguments() {
        AppCompatDelegate.requested = LocaleListCompat.forLanguageTags("en")
        context.resources.configuration.locales = LocaleListCompat.forLanguageTags("ru")
        context.formats[7] = "%s:%d"
        assertSame(context, AppLanguage.context())
        assertEquals("ru:8", AppLanguage.text(8))
        assertEquals("Имя:3", AppLanguage.text(7, "Имя", 3))
    }

    @Test fun textAndContextFailuresKeepTheirCauseAndResourceContext() {
        ContextCompat.failContext = true
        assertThrows(IllegalStateException::class.java) { AppLanguage.context() }
        error("Localized context unavailable")
        ContextCompat.failContext = false; context.failText = true
        assertThrows(IllegalStateException::class.java) { AppLanguage.text(7) }
        error("resource=7")
    }

    @Test fun backgroundSelectionIsRejectedBeforeChangingFrameworkState() {
        val failure = AtomicReference<Throwable?>()
        val worker = Thread { failure.set(runCatching { AppLanguage.select("ru") }.exceptionOrNull()) }.apply { start() }
        worker.join(2000); assertFalse(worker.isAlive)
        assertTrue(failure.get() is IllegalStateException); assertTrue(AppCompatDelegate.selections.isEmpty())
        error("main thread")
    }
}
