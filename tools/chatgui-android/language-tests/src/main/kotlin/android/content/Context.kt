package android.content

import androidx.core.os.LocaleListCompat
import java.io.File
import java.io.StringReader
import javax.xml.parsers.DocumentBuilderFactory
import org.w3c.dom.Element
import org.xml.sax.InputSource
import org.xmlpull.v1.XmlPullParser

/** Только host: effective resources меняются явно, без перезапуска служб/Android configuration. */
class Context {
    val resources = HostResources()
    var failText = false
    val formats = mutableMapOf<Int, String>()
    fun getString(id: Int, vararg args: Any): String {
        check(!failText) { "localized text unavailable" }
        return String.format(java.util.Locale.ROOT, formats[id] ?: "${resources.configuration.locales.toLanguageTags()}:$id", *args)
    }
}

class HostResources {
    private val current = HostConfiguration()
    var failConfiguration = false
    val configuration: HostConfiguration get() { check(!failConfiguration) { "configuration unavailable" }; return current }
    var xml: String? = null
    var failOpen = false
    var failNext = false
    var opened = 0
    var closed = 0
    fun getXml(id: Int): HostXmlParser {
        check(!failOpen) { "locale resource unavailable id=$id" }
        val text = xml ?: File(requireNotNull(System.getProperty("utun.localesConfig"))).readText()
        val factory = DocumentBuilderFactory.newInstance().apply { isNamespaceAware = true }
        val elements = factory.newDocumentBuilder().parse(InputSource(StringReader(text))).getElementsByTagName("*")
        opened++
        return HostXmlParser((0 until elements.length).map { elements.item(it) as Element }, this)
    }
}

class HostConfiguration { var locales = LocaleListCompat.forLanguageTags("en") }

/** Минимальная поверхность XmlResourceParser: закрытие учитывается также при исключении next()/чтении атрибута. */
class HostXmlParser(private val elements: List<Element>, private val owner: HostResources) : AutoCloseable {
    private var index = -1
    var eventType = 0
        private set
    val name: String get() = elements[index].tagName
    fun next(): Int {
        check(!owner.failNext) { "locale XML read failed" }
        index++
        eventType = if (index < elements.size) XmlPullParser.START_TAG else XmlPullParser.END_DOCUMENT
        return eventType
    }
    fun getAttributeValue(namespace: String, name: String): String? = elements[index].getAttributeNodeNS(namespace, name)?.value
    override fun close() { owner.closed++ }
}
