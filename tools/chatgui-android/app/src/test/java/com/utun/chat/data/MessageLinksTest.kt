package com.utun.chat.data

import java.io.File
import org.junit.Assert.*
import org.junit.Test

/** Общие с Qt адреса и диапазоны UTF-16; разбор длинного хвоста не задерживает UI. */
class MessageLinksTest {
    private fun assertLinks(text: String, expected: List<String>) {
        val links = MessageLinks.parse(text)
        assertEquals(text.take(200), expected, links.map { it.url })
        var end = 0
        for (link in links) {
            assertTrue(link.start >= end)
            assertEquals(link.url, text.substring(link.start, link.end))
            end = link.end
        }
    }

    @Test fun sharedQtAndAndroidCases() {
        val fixture = File(requireNotNull(System.getProperty("utun.messageLinkCases")))
        var count = 0
        fixture.forEachLine { line ->
            if (line.isNotEmpty() && !line.startsWith("#")) {
                val columns = line.split('\t')
                assertLinks(columns.first().replace("\\n", "\n"), columns.drop(1))
                count++
            }
        }
        assertTrue("Expected shared link cases", count >= 15)
    }

    @Test fun openingOnlyAcceptsCompleteWebUrls() {
        for (url in listOf("http://example.com/", "HTTPS://пример.рф/путь", "https://[::1]:65535/",
            "http://example.com:0/", "http://example.com:/", "http://example.com:00080/")) {
            assertTrue(url, MessageLinks.isWebUrl(url))
        }
        for (url in listOf("utun://abc+/==", "ftp://example.com/", "https://", "https://example.com/a b", "https://example.com/%GG")) {
            assertFalse(url, MessageLinks.isWebUrl(url))
        }
    }

    @Test fun malformedAuthorityIsNeitherAnnotatedNorOpened() {
        for (url in listOf("http://example.com:-1/", "http://example.com:+80/", "http://example.com:-0/", "http://example.com\\foo/")) {
            assertFalse(url, MessageLinks.isWebUrl(url))
            assertLinks(url, emptyList())
        }
    }

    @Test fun bracketTypesHaveIndependentBalances() {
        for (path in listOf("a(b)", "a[0]", "a{0}", "a([{}])", "a((b)", "a[b[0]")) {
            val url = "https://example.com/$path"
            assertLinks("✨ $url.,;:!?", listOf(url))
        }
        assertLinks("https://example.com/a(b)))! next", listOf("https://example.com/a(b)"))
        assertLinks("https://example.com/a[0]}).", listOf("https://example.com/a[0]"))
        assertLinks("https://example.com/a)(b)", listOf("https://example.com/a)(b"))
    }

    @Test(timeout = 5_000) fun longMixedTailKeepsOriginalRangesWithoutRepeatedCopies() {
        val first = "https://example.com/a([{}])"
        val second = "http://example.org/b"
        val text = "👋 $first" + ")]},.!?".repeat(20_000) + " $second"
        assertLinks(text, listOf(first, second))
        assertEquals(3, MessageLinks.parse(text).first().start)
    }

    @Test fun textWithoutLinksStaysUnannotated() {
        for (text in listOf("", "Обычный текст 👋", "www.example.com", "http:// https://?x=1")) {
            assertLinks(text, emptyList())
        }
    }
}
