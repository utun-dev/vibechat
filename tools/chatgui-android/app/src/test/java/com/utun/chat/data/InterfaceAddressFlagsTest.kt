package com.utun.chat.data

import java.net.InetAddress
import org.junit.Assert.*
import org.junit.Test

class InterfaceAddressFlagsTest {
    @Test fun compressedIpv6MustMatchKernelBytesRatherThanPaddingConcatenatedGroups() {
        val address = InetAddress.getByName("2001:db8::1234").address
        assertTrue(temporaryIpv6Address("wlan0", address, sequenceOf("20010db8000000000000000000001234 02 40 00 20 wlan0")))
    }
    @Test fun anotherInterfaceCannotMarkTheCurrentAddressTemporary() {
        val address = InetAddress.getByName("2001:db8::1234").address
        val rows = listOf("20010db8000000000000000000001234 02 40 00 20 mobile0",
            "20010db8000000000000000000001234 03 40 00 80 wlan0")
        assertFalse(temporaryIpv6Address("wlan0", address, rows.asSequence()))
    }
    @Test fun stableAddressDoesNotInheritFlagsFromAnotherAddressOnTheSameInterface() {
        val rows = listOf("20010db8000000000000000000001234 02 40 00 20 wlan0",
            "20010DB8000000000000000000005678 02 40 00 80 wlan0")
        assertFalse(temporaryIpv6Address("wlan0", InetAddress.getByName("2001:db8::5678").address, rows.asSequence()))
    }
}
