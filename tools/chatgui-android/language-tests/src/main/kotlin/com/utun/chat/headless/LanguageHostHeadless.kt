package com.utun.chat.headless

import com.utun.chat.data.HostLanguageNotifications

object HeadlessService { fun refreshLanguage() = HostLanguageNotifications.refresh("network") }
