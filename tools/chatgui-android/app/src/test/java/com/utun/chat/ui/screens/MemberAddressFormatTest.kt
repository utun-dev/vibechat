package com.utun.chat.ui.screens

import com.utun.chat.R
import com.utun.chat.data.MemberAddr
import org.junit.Assert.assertEquals
import org.junit.Test

class MemberAddressFormatTest {
    @Test fun ipv6EndpointsKeepPortSeparateFromAddress() {
        assertEquals("[2001:db8::1]:443", "${formatAddr("2001:db8::1")}:443")
        assertEquals("[fe80::1%wlan0]:9000", "${formatAddr("fe80::1%wlan0")}:9000")
        assertEquals("[::]:9000", "${formatAddr("[::]")}:9000")
        assertEquals("192.168.1.10:9000", "${formatAddr("192.168.1.10")}:9000")
    }

    @Test fun interfaceAddressKeepsItsMeasuredNatResult() {
        val address = MemberAddr(addrType = 0)
        assertEquals(R.string.socket_nat_unmeasured, address.natLabelRes)
        assertEquals(R.string.socket_no_nat, address.copy(natMapping = 1, natSource = 1).natLabelRes)
    }
}
