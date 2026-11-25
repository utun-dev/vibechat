package com.utun.chat.data

import android.content.Context
import android.os.Looper
import androidx.annotation.StringRes
import androidx.appcompat.app.AppCompatDelegate
import androidx.core.content.ContextCompat
import androidx.core.os.LocaleListCompat
import com.utun.chat.ChatApplication
import com.utun.chat.R
import com.utun.chat.headless.HeadlessService
import org.xmlpull.v1.XmlPullParser
import java.util.Locale

/** Android/AppCompat хранит выбранные языки, locales_config.xml задаёт общий каталог приложения/системных настроек.
 * select/applied требуют main; context/text доступны вне Activity, в том числе сервисам при холодном запуске.
 * Выбор не дублируется в DataStore и не перезапускает ядро/медиа. Применение конфигурации пересоздаёт Activity.
 * Каждый applied обновляет уведомления заново: одинаковый locale не доказывает успех прежней попытки или наличие нового владельца. */
object AppLanguage {
    /** Нормализованные BCP47-теги в порядке XML, кэшируются только после успешного чтения; ошибка логируется/пробрасывается. */
    val supportedTags: List<String> by lazy { readSupportedTags() }

    /** Прочитать/проверить каталог атомарно и закрыть parser также при ошибке; lazy-провал допускает следующую попытку. */
    internal fun readSupportedTags(): List<String> = try {
        ChatApplication.instance.resources.getXml(R.xml.locales_config).use { parser ->
            val tags = buildList {
                while (parser.next() != XmlPullParser.END_DOCUMENT) {
                    if (parser.eventType == XmlPullParser.START_TAG && parser.name == "locale") {
                        val tag = requireNotNull(parser.getAttributeValue("http://schemas.android.com/apk/res/android", "name")) {
                            "locale has no android:name"
                        }
                        require(tag.isNotBlank()) { "empty locale tag" }
                        val locale = Locale.Builder().setLanguageTag(tag).build()
                        require(locale.language.isNotEmpty()) { "locale has no language: tag=$tag" }
                        add(locale.toLanguageTag())
                    }
                }
            }
            require(tags.isNotEmpty()) { "locale catalog is empty" }
            require(tags.distinct().size == tags.size) { "duplicate locale tags: $tags" }
            LogManager.addLog("DEBUG", "LANGUAGE", "Supported language catalog loaded: $tags")
            tags
        }
    } catch (error: Exception) {
        LogManager.addLog("ERROR", "LANGUAGE", "Language catalog load failed: $error")
        throw error
    }

    /** После Activity.onCreate: запрошенный Android/AppCompat список через запятую; пусто — система, не effective язык ресурсов.
     * Ошибка чтения логируется и передаётся вызывающему, не маскируется под системный выбор. */
    fun selectedTag(): String = try { AppCompatDelegate.getApplicationLocales().toLanguageTags()
    } catch (error: Exception) {
        LogManager.addLog("ERROR", "LANGUAGE", "Application language selection read failed: $error"); throw error
    }

    /** На main запросить один supported tag либо пустую строку (система).
     * true — уже выбран/запрос принят AppCompat, false — неподдержанный тег или ошибка; это не подтверждение смены ресурсов. */
    fun select(tag: String): Boolean {
        checkMain()
        return try {
            if (tag.isNotEmpty() && tag !in supportedTags) {
                LogManager.addLog("ERROR", "LANGUAGE", "Unsupported language tag=$tag supported=$supportedTags")
                return false
            }
            val previous = selectedTag()
            if (previous == tag) return true
            AppCompatDelegate.setApplicationLocales(LocaleListCompat.forLanguageTags(tag))
            LogManager.addLog("INFO", "LANGUAGE", "Language selection requested: ${previous.ifEmpty { "system" }} -> ${tag.ifEmpty { "system" }}")
            true
        } catch (error: Exception) {
            LogManager.addLog("ERROR", "LANGUAGE", "Language selection failed: tag=$tag: $error")
            false
        }
    }

    /** Названия BCP47-тега/списка через запятую на собственных языках; неизвестное название заменяется исходным тегом.
     * Для пустого запроса возвращает пусто: локализованной меткой «Система» владеет UI. */
    fun nativeName(tag: String): String = tag.split(',').joinToString(", ") { item ->
        val locale = Locale.forLanguageTag(item)
        locale.getDisplayName(locale).ifEmpty { item }.replaceFirstChar { it.titlecase(locale) }
    }

    /** Локализованный Context для Application/Service без Activity; ошибки Android логируются и передаются владельцу. */
    fun context(): Context = try { ContextCompat.getContextForLanguage(ChatApplication.instance)
    } catch (error: Exception) {
        LogManager.addLog("ERROR", "LANGUAGE", "Localized context unavailable: $error"); throw error
    }

    /** Строка текущих ресурсов с форматированием; ошибки контекста/ресурса не подменяются текстом другого языка. */
    fun text(@StringRes id: Int, vararg args: Any): String {
        val ctx = context()
        return try { ctx.getString(id, *args)
        } catch (error: Exception) {
            LogManager.addLog("ERROR", "LANGUAGE", "Localized text unavailable: resource=$id arguments=${args.size}: $error"); throw error
        }
    }

    /** На main после сигнала конфигурации/создания Activity обновить только каналы/тексты текущих уведомлений.
     * Повтор с тем же effective locale нужен после отказа, в том числе обработанного внутри владельца.
     * Отказы изолированы; при ошибке чтения контекста/конфигурации сигнал логируется и пропускается до следующего вызова. */
    fun applied() {
        checkMain()
        val locales = try { context().resources.configuration.locales.toLanguageTags()
        } catch (error: Exception) {
            LogManager.addLog("ERROR", "LANGUAGE", "Notification language configuration unavailable: $error"); return
        }
        LogManager.addLog("DEBUG", "LANGUAGE", "Notification language refresh requested: locales=$locales")
        val updates = listOf(
            "network" to { HeadlessService.refreshLanguage() },
            "incoming call" to { IncomingCallManager.refreshLanguage() },
            "ongoing call" to { CallController.refreshLanguage() },
            "radio" to { RadioOverlayService.refreshLanguage() }
        )
        updates.forEach { (name, update) ->
            try { update() }
            catch (e: Exception) {
                LogManager.addLog("ERROR", "LANGUAGE", "Notification language refresh failed: owner=$name locales=$locales: $e")
            }
        }
    }

    private fun checkMain() {
        check(Looper.myLooper() === Looper.getMainLooper()) {
            val message = "AppLanguage requires main thread: caller=${Thread.currentThread().name}"
            LogManager.addLog("ERROR", "LANGUAGE", message); message
        }
    }
}
