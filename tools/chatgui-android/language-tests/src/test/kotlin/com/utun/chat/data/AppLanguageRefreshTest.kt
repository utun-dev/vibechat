package com.utun.chat.data

import android.content.Context
import android.os.Looper
import androidx.appcompat.app.AppCompatDelegate
import androidx.core.content.ContextCompat
import androidx.core.os.LocaleListCompat
import com.utun.chat.ChatApplication
import java.util.concurrent.atomic.AtomicReference
import org.junit.Assert.*
import org.junit.Before
import org.junit.Test

/** Реальный applied(); владельцы уведомлений считают независимые попытки, не запускают службы/медиа. */
class AppLanguageRefreshTest {
    private val owners = listOf("network", "incoming call", "ongoing call", "radio")
    private lateinit var context: Context
    companion object { private var sequence = 0 }

    @Before fun prepare() {
        assertSame(Looper.getMainLooper(), Looper.myLooper())
        context = Context(); ChatApplication.instance = context
        context.resources.configuration.locales = LocaleListCompat.forLanguageTags("en-x-test-${++sequence}")
        ContextCompat.failContext = false
        AppCompatDelegate.failRead = false; AppCompatDelegate.failWrite = false
        AppCompatDelegate.failWriteAfter = false
        AppCompatDelegate.requested = LocaleListCompat.forLanguageTags("ru")
        AppCompatDelegate.selections.clear()
        HostLanguageNotifications.attempts.clear(); HostLanguageNotifications.failures.clear()
        LogManager.entries.clear()
    }

    @Test fun dispatchesAllOwnersUsingEffectiveResourcesWithoutChangingSelection() {
        AppLanguage.applied()
        assertEquals(owners, HostLanguageNotifications.attempts)
        assertTrue(AppCompatDelegate.selections.isEmpty())
        assertEquals("ru", AppCompatDelegate.requested.toLanguageTags())
        val effective = context.resources.configuration.locales.toLanguageTags()
        assertTrue(LogManager.entries.any { effective in it.message })
    }

    @Test fun everyFailedOwnerCanBeRetriedWithTheSameLocales() {
        for ((index, owner) in owners.withIndex()) {
            context.resources.configuration.locales = LocaleListCompat.forLanguageTags("en-x-retry-${sequence}-$index")
            HostLanguageNotifications.failures.add(owner)
            HostLanguageNotifications.attempts.clear()
            AppLanguage.applied()
            assertEquals(owners, HostLanguageNotifications.attempts)
            assertTrue(LogManager.entries.any { it.level == "ERROR" && owner in it.message })
            HostLanguageNotifications.failures.clear(); HostLanguageNotifications.attempts.clear()
            AppLanguage.applied()
            assertEquals("owner=$owner", owners, HostLanguageNotifications.attempts)
        }
    }

    @Test fun sameLocalesStillReachOwnersThatFailedInternallyOrBecameActiveLater() {
        AppLanguage.applied()
        HostLanguageNotifications.attempts.clear()
        AppLanguage.applied()
        assertEquals(owners, HostLanguageNotifications.attempts)
    }

    @Test fun allOwnerFailuresAreIsolatedAndLogged() {
        HostLanguageNotifications.failures.addAll(owners)
        assertNull(runCatching { AppLanguage.applied() }.exceptionOrNull())
        assertEquals(owners, HostLanguageNotifications.attempts)
        assertEquals(4, LogManager.entries.count { it.level == "ERROR" && "notification" in it.message.lowercase() })
    }

    @Test fun contextFailureIsReportedWithoutEscapingConfigurationCallback() {
        ContextCompat.failContext = true
        assertNull(runCatching { AppLanguage.applied() }.exceptionOrNull())
        assertTrue(HostLanguageNotifications.attempts.isEmpty())
        assertTrue(LogManager.entries.any { it.level == "ERROR" && "context unavailable" in it.message })
        ContextCompat.failContext = false
        AppLanguage.applied()
        assertEquals(owners, HostLanguageNotifications.attempts)
    }

    @Test fun configurationFailureDoesNotPoisonTheNextSignal() {
        context.resources.failConfiguration = true
        assertNull(runCatching { AppLanguage.applied() }.exceptionOrNull())
        assertTrue(HostLanguageNotifications.attempts.isEmpty())
        assertTrue(LogManager.entries.any { it.level == "ERROR" && "configuration unavailable" in it.message })
        context.resources.failConfiguration = false
        AppLanguage.applied()
        assertEquals(owners, HostLanguageNotifications.attempts)
    }

    @Test fun backgroundSignalIsRejectedBeforeCallingAnyOwner() {
        val failure = AtomicReference<Throwable?>()
        val worker = Thread { failure.set(runCatching { AppLanguage.applied() }.exceptionOrNull()) }.apply { start() }
        worker.join(2000); assertFalse(worker.isAlive)
        assertTrue(failure.get() is IllegalStateException)
        assertTrue(HostLanguageNotifications.attempts.isEmpty())
        assertTrue(LogManager.entries.any { it.level == "ERROR" && "main thread" in it.message })
    }
}
