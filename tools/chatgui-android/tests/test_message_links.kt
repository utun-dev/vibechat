import com.utun.chat.data.MessageLinks
import java.io.File

/** Один набор адресов и границ для Qt и Android, включая UTF-16 после emoji. */
fun main(args: Array<String>) {
    var count = 0
    File(args.single()).readLines().filter { it.isNotEmpty() && !it.startsWith("#") }.forEach { line ->
        val columns = line.split('\t')
        val text = columns.first().replace("\\n", "\n")
        val links = MessageLinks.parse(text)
        check(links.map { it.url } == columns.drop(1)) { "URLs differ: text=$text actual=$links expected=${columns.drop(1)}" }
        var end = 0
        for (link in links) {
            check(link.start >= end && text.substring(link.start, link.end) == link.url)
            end = link.end
        }
        count++
    }
    check(count >= 15)
    println("PASS: $count shared message-link cases; original URLs and UTF-16 ranges preserved")
}
