# Tests Makefile.am for utun - all tests with new ll_queue library

EXTRA_DIST = bench_etcp_packet_cpu.py fixtures/reality_openssl_hello.hex data/silero_speech.pcm data/silero_speech.txt test_net.h test_membership.h test_join_id.h test_member_staging.h nat_fixture.h db_sync_fixture.h \
    media_members_fixture.h nat_test_key.h stream_address_fixture.h run_tests.py test_run_tests.py test_service_ids.py fixtures/group_isolation_matrix.txt test_build_script.py proxy_rolling_load.py \
    test_prepare_dependencies.py test_proxy_env.py test_build_linux_full.py test_prepare_android.py \
    local_transport_ipv6.sh stream_netns/topology.py stream_netns/network.py \
    stream_netns/probe.py stream_netns/run.py stream_netns/scenario.py stream_netns/README.md stream_netns/TESTED.md \
    stream_netns/test_isolation.py stream_netns/test_report.py stream_netns/report.py \
    stream_netns/nat_scenario.py stream_netns/stun_server.py stream_netns/stun_scenario.py

# Порядок по замерам Linux: быстрые проверки раньше длительных; состав зависит от прежних configure-условий.

# Быстрые проверки (до 100мс).
check_PROGRAMS = \
    test_crypto_pool \
    test_packet_transport \
    test_stream_transport \
    test_video \
    test_video_no_ffmpeg \
    test_video_plane \
    test_route_tx_stats \
    test_call_audio_batch \
    test_call_video_rate \
    test_call_video_playout \
    test_call_video_port \
    test_call_video \
    test_call_video_flush \
    test_memory_pool_and_config \
    test_serialize \
    test_reed_solomon \
    bench_uasync_timeouts \
    test_etcp_minimal \
    test_sock_match \
    test_u_async_performance \
    bench_timeout_heap \
    test_config_debug \
    test_debug_categories \
    test_db_changes \
    test_log_rotation \
    test_packet_link \
    test_etcp_packet_link \
    test_etcp_driver \
    test_etcp_tx_rto \
    test_etcp_bbr \
    test_packet_dump \
    test_radio_vad \
    test_media_delivery_sql \
    test_etcp_crypto \
    test_http_proxy \
    test_route_lib \
    test_reality_hello \
    test_stream_cipher \
    test_reality_record \
    test_stream_sign \
    test_stream_control_expiry \
    test_stream_budget \
    test_stream_standby \
    test_stream_paths \
    test_stream_live \
    test_reality_config \
    test_socks_client \
    test_dm \
    test_nat_engine \
    test_opus_codec \
    test_member_files \
    test_attachment \
    test_media_index \
    test_etcp_lifecycle \
    test_etcp_server_lifecycle \
    test_reality_bgp \
    test_tcp_proxy_client \
    test_etcp_session \
    test_etcp_init_liveness \
    test_proxy_packets \
    test_media_delivery_download \
    test_transport \
    test_etcp_router_unit \
    test_multicast \
    test_group_ownership \
    test_merkle_protocol \
    test_member_versions \
    test_chat_sync_schedule \
    test_ncd_config \
    test_node_snapshot \
    test_node_record_async \
    test_node_rtt \
    test_core_db_async \
    test_nat_history_async \
    test_auto_ports_async \
    test_ntp_offsets_async \
    test_stream_catalog_async \
    test_radio_mute_async \
    test_call_targets_async \
    test_headless_db_async \
    test_dm_conversations_async \
    test_dm_receipt_async \
    test_dm_start_async \
    test_dm_send_async \
	test_dm_receive_async \
    test_dm_download_async \
    test_dm_teardown_async \
    test_dm_outbox_async \
    test_group_access_async \
    test_kanban_read_async \
    test_kanban_write_async \
    test_member_read_async \
    test_group_info_read_async \
    test_group_policy_read_async \
    test_topo_node_wire \
    test_route6_lib \
    test_etcp_transport \
    test_stcp_ping \
    test_intensive_memory_pool \
    test_ll_queue \
    test_invite_codec \
    test_object_link \
    test_chat_join_faults \
    test_nat_transport \
    test_conn_mgr_already_connected \
    test_udp_proxy \
    test_icmp_proxy \
    test_chat_poll \
    test_pkt_normalizer_standalone \
    test_services \
    test_etcp_ping \
    test_etcp_router_faults \
    test_etcp_seq_collision \
    test_conn_mgr \
    test_ntp \
    test_route_ping \
    test_tcp_proxy_server \
    test_db_sync_schema \
    test_member_adapter \
    test_group_policy \
    test_chat_feedback \
    test_encrypted_file \
    test_encrypted_attachment \
    test_uasync_socket_race \
    test_etcp_two_instances \
    test_reality_tls \
    test_radix \
    test_daemon_worker \
    test_attachment_send \
    test_group_recovery \
    test_conn_mgr_handles \
    test_tcp_io_flush

if OS_LINUX
check_PROGRAMS += \
    test_ntp_regressions \
    test_etcp_windows \
    test_dummynet_channels \
    test_ncd_faults \
    test_stcp_sigpipe \
    test_firewall \
    test_mem \
    test_mem_threads \
    test_config_updater \
    test_stun_nat \
    test_chat_admin \
    test_chat_attachment \
    test_attachment_results \
    test_attachment_decisions \
    test_attachment_requests \
    test_db_sync_integrity \
    test_reality_fallback \
    test_etcp_padding \
    test_broadcast_cache \
    test_net_emulator \
    test_ll_queue_regressions \
    test_standby \
    test_standby_transport \
    test_tun_route \
    test_tun_if
endif

# Короткие интеграционные проверки (до 1с).
check_PROGRAMS += \
    test_stream_core \
    test_stream_media \
    test_stream_playback \
    test_stream_tx_stats \
    test_android_stream \
    test_stream_service \
    test_stream_failover \
    test_stream_overload \
    test_stream_offline \
    test_stream_member \
    test_stream_relay \
    test_stream_publisher \
    test_call_admission \
    test_u_async_comprehensive \
    test_nat_detection \
    test_audio_compressor \
    test_etcp_simple_traffic \
    test_pkt_normalizer_etcp \
    test_broadcast \
    test_chat_group_isolation \
    test_group_api_isolation \
    test_stcp_traffic \
    test_etcp_100_packets \
    test_topo_recovery \
    test_group_exchange \
    test_etcp_router_reconnect \
    test_ipv6_sockets \
    test_radio_headless \
    test_radio_mesh \
    test_radio_batch \
    test_nat_stress \
    test_stcp \
    test_call_headless \
    test_media_delivery_full \
    test_media_download_timeout \
    test_etcp_api \
    test_tcp_io \
    test_lwip_tcp \
    test_speex_aec \
    test_speex_ns \
    test_db_sync \
    test_media_delivery_integration \
    test_bgp_paths \
    test_dm_invite \
    test_etcp_link_stress \
    test_etcp_router_tcp \
    test_proxy_regressions \
    test_etcp_connect \
    test_call \
    test_group_connect \
    test_etcp_router \
    test_dm_worker \
    test_dm_history \
    test_dm_names \
    test_chat_join \
    test_radio \
    test_bgp_route_exchange \
    test_chat_sync_stress \
    test_node_conn_direct

if OS_LINUX
check_PROGRAMS += \
    test_uasync_regressions \
    test_dm_load \
    test_stcp_reality_rotation \
    test_android_udp_log \
    test_pool_corruption_log \
    test_group_connect_standby \
    test_android_replies
endif

# Проверки до 3с.
check_PROGRAMS += \
    test_u_async_timeouts \
    test_async_dns \
    test_chat_poll_network \
    test_group_avatar \
    test_dm_e2e \
    test_media_async \
    test_bgp_triangle \
    test_chat_join_e2e \
    test_media_delivery_chat \
    test_member_files_network \
    test_etcp_reconnect \
    test_conn_mgr_phases \
    test_socks_http_proxy

# Длительные проверки и нагрузка.
check_PROGRAMS += \
    test_bbr_integration \
    test_dm_media \
    test_merkle_sync

if OS_LINUX
check_PROGRAMS += \
    test_uasync_load \
    test_auto_socket_dynamic
endif

# ONNX Runtime: без измерений относим дополнительный VAD-тест к длительным.
if HAVE_SILERO_VAD
check_PROGRAMS += test_silero_vad
endif

# Долгие тесты: запускаются только вручную
EXTRA_PROGRAMS = test_merkle_sync_stress
CLEANFILES = test_merkle_sync_stress$(EXEEXT)
if OS_LINUX
EXTRA_PROGRAMS += test_etcp_multipath
CLEANFILES += test_etcp_multipath$(EXEEXT)
endif

if OS_LINUX
# Пропускная способность/CPU обычного ядра: два процесса и независимые crypto workers.
EXTRA_PROGRAMS += bench_etcp_throughput
bench_etcp_throughput_SOURCES = bench_etcp_throughput.c
bench_etcp_throughput_LDADD = $(LIBUTUN) $(COMMON_LIBS)
CLEANFILES += bench_etcp_throughput$(EXEEXT)

# Профилируемые объекты отдельны от libutun: обычная сборка не читает CPU-часы на пути пакета.
EXTRA_PROGRAMS += bench_etcp_packet_cpu
bench_etcp_packet_cpu_SOURCES = bench_etcp_packet_cpu.c \
    ../src/transport_layer/etcp_profile.h ../src/transport_layer/etcp_api.c \
    ../src/transport_layer/etcp.c ../src/transport_layer/pkt_normalizer.c ../src/transport_layer/etcp_connections.c \
    ../src/transport_layer/etcp_loadbalancer.c ../src/transport_layer/secure_channel.c
bench_etcp_packet_cpu_CPPFLAGS = $(AM_CPPFLAGS) -DETCP_PACKET_PROFILE
bench_etcp_packet_cpu_LDADD = $(LIBUTUN) $(COMMON_LIBS)
bench_etcp_packet_cpu_LDFLAGS = -Wl,--wrap=socket_sendto -Wl,--wrap=u_malloc_impl -Wl,--wrap=u_calloc_impl \
    -Wl,--wrap=u_realloc_impl -Wl,--wrap=u_free_impl -Wl,--wrap=u_strdup_impl -Wl,--wrap=u_check \
    -Wl,--wrap=u_get_allocated_count -Wl,--wrap=u_report_unfreed_blocks
CLEANFILES += bench_etcp_packet_cpu$(EXEEXT)
endif

if OS_LINUX
# Отдельный привилегированный стенд не запускается обычным make check.
EXTRA_PROGRAMS += stream_netns/stream_lab_node
stream_netns_stream_lab_node_SOURCES = stream_netns/node.c
stream_netns_stream_lab_node_LDADD = $(LIBUTUN) $(CRYPTO_LIBS) $(COMMON_LIBS)

check-stream-netns: stream_netns/stream_lab_node$(EXEEXT)
	python3 "$(srcdir)/stream_netns/test_isolation.py"
	python3 "$(srcdir)/stream_netns/test_report.py"
	python3 "$(srcdir)/stream_netns/run.py" --binary "$(abs_builddir)/stream_netns/stream_lab_node$(EXEEXT)" $(STREAM_NETNS_ARGS)
endif

# Разрывы двух узлов и race трёх SQLite-баз; не входит в all/check/check.sh.
.PHONY: check-merkle-sync-stress check-stream-netns
check-merkle-sync-stress: test_merkle_sync_stress$(EXEEXT)
	./test_merkle_sync_stress$(EXEEXT)

test_merkle_sync_stress_SOURCES = test_merkle_sync_stress.c
test_merkle_sync_stress_LDADD = $(LIBUTUN) $(COMMON_LIBS)

# Basic includes
AM_CFLAGS = -g
AM_CPPFLAGS = -I$(top_srcdir)/src -I$(top_srcdir)/src/transport_layer -I$(top_srcdir)/src/routing_layer -I$(top_srcdir)/lib -I$(srcdir)
LIBUTUN = $(top_builddir)/src/libutun.a

# Windows-specific libraries
if OS_WINDOWS
WIN_LIBS = -lws2_32 -liphlpapi -ladvapi32 -lbcrypt -ldbghelp
else
WIN_LIBS =
endif

# Crypto always uses OpenSSL
CRYPTO_LIBS = -lcrypto

# Common libraries (lib/dns.c собирается с -DDNS_RANDOM=RAND_bytes → нужен -lcrypto)
COMMON_LIBS = $(top_builddir)/lib/libuasync.a $(CRYPTO_LIBS) -lpthread $(WIN_LIBS) @FFMPEG_LIBS@ @WHISPER_LIBS@ $(top_builddir)/lib/libopus/libopus_internal.a @SILERO_VAD_LIBS@ -lm

test_clock_sources = test_clock.c test_clock.h
if OS_LINUX
test_clock_ldflags = -Wl,--wrap=clock_gettime -Wl,--wrap=gettimeofday -Wl,--wrap=epoll_wait -Wl,--wrap=poll
else
test_clock_ldflags =
endif

if !OS_WINDOWS
check_PROGRAMS += test_socket_monitor
check_PROGRAMS += test_reality_relay_faults
test_reality_relay_faults_SOURCES = test_reality_relay_faults.c
test_reality_relay_faults_LDADD = $(LIBUTUN) $(COMMON_LIBS)
test_reality_relay_faults_LDFLAGS = -Wl,--wrap=uasync_set_timeout -Wl,--wrap=uasync_call_soon \
    -Wl,--wrap=socket_set_nonblocking -Wl,--wrap=uasync_set_socket_read
check_PROGRAMS += test_call_ring
test_call_ring_SOURCES = test_call_ring.c ../src/transport_layer/call_ring.c
test_call_ring_LDADD = $(COMMON_LIBS)
test_call_ring_LDFLAGS = -Wl,--wrap=write
test_socket_monitor_SOURCES = test_socket_monitor.c
test_socket_monitor_LDADD = $(LIBUTUN) $(COMMON_LIBS)
test_socket_monitor_LDFLAGS = -Wl,--wrap=socket -Wl,--wrap=bind -Wl,--wrap=close -Wl,--wrap=socket_set_nonblocking \
    -Wl,--wrap=uasync_add_socket -Wl,--wrap=uasync_remove_socket -Wl,--wrap=u_calloc_impl \
    -Wl,--wrap=get_interface_ip_by_index -Wl,--wrap=get_interface_ipv6_addr_nl \
    -Wl,--wrap=get_default_route_netif_index -Wl,--wrap=get_outgoing_local_ip -Wl,--wrap=etcp_socket_cbk_fire
if OS_LINUX
test_socket_monitor_LDFLAGS += -Wl,--wrap=recvfrom -Wl,--wrap=__recvfrom_chk
check_PROGRAMS += test_config_parser
check_PROGRAMS += test_chat_setting
check_PROGRAMS += test_chat_channel
check_PROGRAMS += test_chat_log test_member_log test_kanban test_kanban_merge test_group_info
check_PROGRAMS += test_db_async test_db_sql test_db_transaction test_chat_state_async test_member_hash_async test_member_log_async test_chat_message_async
test_member_log_async_SOURCES = test_member_log_async.c
test_member_log_async_LDADD = $(LIBUTUN) $(COMMON_LIBS)
test_db_changes_SOURCES = test_db_changes.c
test_db_changes_LDADD = $(LIBUTUN) $(COMMON_LIBS)
test_db_async_SOURCES = test_db_async.c
test_db_async_LDADD = $(LIBUTUN) $(COMMON_LIBS)
test_db_async_LDFLAGS = -Wl,--wrap=u_calloc_impl -Wl,--wrap=u_strdup_impl -Wl,--wrap=queue_new \
    -Wl,--wrap=queue_entry_new -Wl,--wrap=queue_data_put -Wl,--wrap=pthread_mutex_init \
    -Wl,--wrap=pthread_cond_init -Wl,--wrap=pthread_create -Wl,--wrap=sqlite3_exec -Wl,--wrap=sqlite3_close \
    -Wl,--wrap=sqlite3_initialize -Wl,--wrap=sqlite3_randomness
test_db_sql_SOURCES = test_db_sql.c
test_db_sql_LDADD = $(LIBUTUN) $(COMMON_LIBS)
test_db_sql_LDFLAGS = -Wl,--wrap=u_calloc_impl -Wl,--wrap=u_realloc_impl -Wl,--wrap=u_malloc_impl \
    -Wl,--wrap=sqlite3_prepare_v2 -Wl,--wrap=sqlite3_bind_int64 -Wl,--wrap=sqlite3_step \
    -Wl,--wrap=sqlite3_finalize -Wl,--wrap=sqlite3_exec
test_db_transaction_SOURCES = test_db_transaction.c
test_db_transaction_LDADD = $(LIBUTUN) $(COMMON_LIBS)
test_db_transaction_LDFLAGS = -Wl,--wrap=u_calloc_impl -Wl,--wrap=u_realloc_impl -Wl,--wrap=u_malloc_impl \
    -Wl,--wrap=queue_entry_new -Wl,--wrap=queue_data_put -Wl,--wrap=sqlite3_prepare_v2 -Wl,--wrap=sqlite3_bind_int64 \
    -Wl,--wrap=sqlite3_step -Wl,--wrap=sqlite3_finalize -Wl,--wrap=sqlite3_exec
test_chat_state_async_SOURCES = test_chat_state_async.c
test_chat_state_async_LDADD = $(LIBUTUN) $(COMMON_LIBS)
test_chat_state_async_LDFLAGS = -Wl,--wrap=sqlite3_prepare_v2 -Wl,--wrap=sqlite3_finalize -Wl,--wrap=sqlite3_exec
test_chat_message_async_SOURCES = test_chat_message_async.c
test_chat_message_async_LDADD = $(LIBUTUN) $(COMMON_LIBS)
test_chat_message_async_LDFLAGS = -Wl,--wrap=sqlite3_prepare_v2 -Wl,--wrap=sqlite3_exec -Wl,--wrap=u_calloc_impl -Wl,--wrap=uasync_set_timeout \
    -Wl,--wrap=media_download_start -Wl,--wrap=sqlite3_finalize
test_member_hash_async_SOURCES = test_member_hash_async.c
test_member_hash_async_LDADD = $(LIBUTUN) $(COMMON_LIBS)
test_member_hash_async_LDFLAGS = -Wl,--wrap=sqlite3_prepare_v2 -Wl,--wrap=sqlite3_step -Wl,--wrap=sqlite3_finalize \
    -Wl,--wrap=sqlite3_column_blob -Wl,--wrap=sqlite3_column_text -Wl,--wrap=sqlite3_column_type
test_member_log_SOURCES = test_member_log.c
test_member_log_CFLAGS = $(test_services_CFLAGS)
test_member_log_LDADD = $(LIBUTUN) $(CRYPTO_LIBS) $(COMMON_LIBS)
test_kanban_SOURCES = test_kanban.c
test_kanban_CFLAGS = $(test_services_CFLAGS)
test_kanban_LDADD = $(LIBUTUN) $(CRYPTO_LIBS) $(COMMON_LIBS)
test_kanban_LDFLAGS = -Wl,--wrap=sqlite3_prepare_v2 -Wl,--wrap=sqlite3_exec
test_kanban_merge_SOURCES = test_kanban_merge.c ../src/kanban/text_merge.c
test_kanban_merge_LDADD = ../lib/libuasync.a $(CRYPTO_LIBS) $(COMMON_LIBS)
test_group_info_SOURCES = test_group_info.c
test_group_info_CFLAGS = $(test_services_CFLAGS)
test_group_info_LDADD = $(LIBUTUN) $(CRYPTO_LIBS) $(COMMON_LIBS)
check_PROGRAMS += test_group_avatar_sql
check_PROGRAMS += test_chat_member
check_PROGRAMS += test_chat_json
check_PROGRAMS += test_chat_profile
check_PROGRAMS += test_chat_profile_async
test_chat_profile_async_SOURCES = test_chat_profile_async.c
test_chat_profile_async_CFLAGS = $(test_core_db_async_CFLAGS)
test_chat_profile_async_LDADD = $(test_core_db_async_LDADD)
if OS_LINUX
test_chat_profile_async_LDFLAGS = -Wl,--wrap=sqlite3_prepare_v2 -Wl,--wrap=sqlite3_exec -Wl,--wrap=u_calloc_impl
endif
check_PROGRAMS += test_chat_preferences
check_PROGRAMS += test_chat_whisper test_chat_whisper_disabled
check_PROGRAMS += test_chat_headless_control
check_PROGRAMS += test_invite_build
check_PROGRAMS += test_invite_async
check_PROGRAMS += test_group_lifecycle_async

test_group_lifecycle_async_SOURCES = test_group_lifecycle_async.c
test_group_lifecycle_async_CFLAGS = $(test_core_db_async_CFLAGS)
test_group_lifecycle_async_LDADD = $(test_core_db_async_LDADD)
test_group_lifecycle_async_LDFLAGS = -Wl,--wrap=sqlite3_prepare_v2 -Wl,--wrap=sqlite3_exec -Wl,--wrap=u_calloc_impl

test_invite_async_SOURCES = test_invite_async.c
test_invite_async_CFLAGS = $(test_core_db_async_CFLAGS)
test_invite_async_LDADD = $(test_core_db_async_LDADD)
test_invite_async_LDFLAGS = -Wl,--wrap=sqlite3_prepare_v2 -Wl,--wrap=sqlite3_exec -Wl,--wrap=sqlite3_bind_blob \
    -Wl,--wrap=sqlite3_step -Wl,--wrap=sqlite3_finalize -Wl,--wrap=u_calloc_impl -Wl,--wrap=u_realloc_impl

test_invite_build_SOURCES = test_invite_build.c
test_invite_build_LDADD = $(LIBUTUN) $(COMMON_LIBS)
test_invite_build_LDFLAGS = -Wl,--wrap=sqlite3_prepare_v2 -Wl,--wrap=sqlite3_bind_blob -Wl,--wrap=sqlite3_step -Wl,--wrap=sqlite3_finalize

test_chat_headless_control_SOURCES = test_chat_headless_control.c
test_chat_headless_control_LDADD = $(LIBUTUN) $(COMMON_LIBS)
test_chat_headless_control_LDFLAGS = -Wl,--wrap=chat_core_submit_media_message -Wl,--wrap=u_calloc_impl \
    -Wl,--wrap=uasync_set_timeout -Wl,--wrap=sqlite3_finalize

test_chat_whisper_SOURCES = test_chat_whisper.c whisper_stub/whisper.h ../src/chat/chat_whisper.c
test_chat_whisper_CPPFLAGS = $(AM_CPPFLAGS) -I$(srcdir)/whisper_stub
test_chat_whisper_CFLAGS = $(AM_CFLAGS) -DHAVE_WHISPER
test_chat_whisper_LDADD = $(LIBUTUN) $(COMMON_LIBS)
test_chat_whisper_LDFLAGS = -Wl,--wrap=u_calloc_impl -Wl,--wrap=u_malloc_impl
test_chat_whisper_disabled_SOURCES = test_chat_whisper_disabled.c ../src/chat/chat_whisper.c
test_chat_whisper_disabled_CFLAGS = $(AM_CFLAGS) -UHAVE_WHISPER
test_chat_whisper_disabled_LDADD = $(LIBUTUN) $(COMMON_LIBS)
check_PROGRAMS += test_chat_lifecycle
check_PROGRAMS += test_instance_contract
test_instance_contract_SOURCES = test_instance_contract.c
test_instance_contract_LDADD = $(LIBUTUN) $(COMMON_LIBS)
test_instance_contract_LDFLAGS = -Wl,--wrap=u_calloc_impl -Wl,--wrap=etcp_create_server_socket -Wl,--wrap=uasync_set_timeout
test_config_parser_SOURCES = test_config_parser.c
test_config_parser_LDADD = $(LIBUTUN) $(COMMON_LIBS)
test_config_parser_LDFLAGS = -Wl,--wrap=u_calloc_impl -Wl,--wrap=u_realloc_impl
test_chat_setting_SOURCES = test_chat_setting.c
test_chat_setting_LDADD = $(LIBUTUN) $(COMMON_LIBS)
test_chat_setting_LDFLAGS = -Wl,--wrap=chat_local_apply_settings
test_chat_channel_SOURCES = test_chat_channel.c
test_chat_channel_LDADD = $(LIBUTUN) $(COMMON_LIBS)
test_chat_channel_LDFLAGS = -Wl,--wrap=EVP_DigestSign -Wl,--wrap=sqlite3_exec -Wl,--wrap=sqlite3_prepare_v2 \
    -Wl,--wrap=sqlite3_bind_blob -Wl,--wrap=sqlite3_bind_text -Wl,--wrap=sqlite3_bind_int64 -Wl,--wrap=sqlite3_bind_null \
    -Wl,--wrap=sqlite3_step -Wl,--wrap=sqlite3_changes -Wl,--wrap=sqlite3_finalize -Wl,--wrap=sqlite3_mprintf \
    -Wl,--wrap=EVP_PKEY_CTX_new_id -Wl,--wrap=EVP_PKEY_keygen_init -Wl,--wrap=EVP_PKEY_keygen \
    -Wl,--wrap=EVP_PKEY_get_raw_public_key -Wl,--wrap=EVP_PKEY_get_raw_private_key
test_group_avatar_sql_SOURCES = test_group_avatar_sql.c
test_group_avatar_sql_LDADD = $(LIBUTUN) $(COMMON_LIBS)
test_group_avatar_sql_LDFLAGS = -Wl,--wrap=sqlite3_bind_blob -Wl,--wrap=sqlite3_bind_int64 -Wl,--wrap=sqlite3_bind_text \
    -Wl,--wrap=sqlite3_exec -Wl,--wrap=sqlite3_prepare_v2 -Wl,--wrap=sqlite3_step -Wl,--wrap=sqlite3_changes -Wl,--wrap=sqlite3_finalize \
    -Wl,--wrap=sc_ed25519_sign
test_chat_member_SOURCES = test_chat_member.c
test_chat_member_LDADD = $(LIBUTUN) $(COMMON_LIBS)
test_chat_member_LDFLAGS = -Wl,--wrap=sqlite3_bind_blob -Wl,--wrap=sqlite3_finalize -Wl,--wrap=sqlite3_prepare_v2 -Wl,--wrap=sqlite3_bind_int64 \
    -Wl,--wrap=sqlite3_bind_int -Wl,--wrap=sqlite3_step -Wl,--wrap=sqlite3_changes -Wl,--wrap=sqlite3_exec \
    -Wl,--wrap=u_malloc_impl -Wl,--wrap=u_realloc_impl
test_chat_json_SOURCES = test_chat_json.c
test_chat_json_LDADD = $(LIBUTUN) $(COMMON_LIBS)
test_chat_json_LDFLAGS = -Wl,--wrap=sqlite3_prepare_v2 -Wl,--wrap=sqlite3_bind_text -Wl,--wrap=sqlite3_bind_int64 -Wl,--wrap=sqlite3_bind_blob \
    -Wl,--wrap=sqlite3_bind_int -Wl,--wrap=sqlite3_step -Wl,--wrap=sqlite3_finalize -Wl,--wrap=sqlite3_column_text \
    -Wl,--wrap=u_realloc_impl -Wl,--wrap=u_calloc_impl
test_chat_preferences_SOURCES = test_chat_preferences.c
test_chat_lifecycle_SOURCES = test_chat_lifecycle.c
test_chat_lifecycle_LDADD = $(LIBUTUN) $(COMMON_LIBS)
test_chat_lifecycle_LDFLAGS = -Wl,--wrap=member_sync_add_props_cbk -Wl,--wrap=utun_add_nodeinfo_cbk \
    -Wl,--wrap=u_malloc_impl -Wl,--wrap=u_calloc_impl -Wl,--wrap=sqlite3_prepare_v2 -Wl,--wrap=sqlite3_bind_text \
    -Wl,--wrap=sqlite3_bind_blob -Wl,--wrap=sqlite3_bind_int64 -Wl,--wrap=sqlite3_step -Wl,--wrap=sqlite3_changes \
    -Wl,--wrap=sqlite3_finalize -Wl,--wrap=sqlite3_exec -Wl,--wrap=sqlite3_get_table -Wl,--wrap=db_sync_instance_activate
test_chat_preferences_LDADD = $(LIBUTUN) $(COMMON_LIBS)
test_chat_preferences_LDFLAGS = -Wl,--wrap=sqlite3_finalize -Wl,--wrap=sqlite3_prepare_v2 -Wl,--wrap=sqlite3_bind_text \
    -Wl,--wrap=sqlite3_step -Wl,--wrap=sqlite3_changes -Wl,--wrap=sqlite3_exec -Wl,--wrap=sqlite3_column_text \
    -Wl,--wrap=chat_sync_broadcast_sound_setting -Wl,--wrap=chat_core_apply_group_autoconnect
test_chat_profile_SOURCES = test_chat_profile.c
test_chat_profile_LDADD = $(LIBUTUN) $(COMMON_LIBS)
test_chat_profile_LDFLAGS = -Wl,--wrap=sqlite3_finalize -Wl,--wrap=sqlite3_prepare_v2 -Wl,--wrap=sqlite3_bind_text \
    -Wl,--wrap=sqlite3_bind_blob -Wl,--wrap=sqlite3_bind_int64 -Wl,--wrap=sqlite3_bind_int -Wl,--wrap=sqlite3_bind_null \
    -Wl,--wrap=sqlite3_step -Wl,--wrap=sqlite3_column_text -Wl,--wrap=sqlite3_exec -Wl,--wrap=sc_ed25519_sign \
    -Wl,--wrap=sc_ed25519_verify -Wl,--wrap=u_malloc_impl -Wl,--wrap=member_sync_add_props_cbk -Wl,--wrap=sqlite3_changes \
    -Wl,--wrap=u_calloc_impl -Wl,--wrap=u_realloc_impl -Wl,--wrap=u_strdup_impl
else
test_socket_monitor_LDFLAGS += -Wl,--wrap=read
endif
endif

# Вариант общего ядра с Android duty-cycle, без JNI и устройства.
if OS_LINUX
check_PROGRAMS += test_local_transport
check_PROGRAMS += test_auto_socket_faults
test_auto_socket_faults_SOURCES = test_auto_socket_faults.c
test_auto_socket_faults_LDADD = $(LIBUTUN) $(COMMON_LIBS)
test_auto_socket_faults_LDFLAGS = -Wl,--wrap=getifaddrs -Wl,--wrap=freeifaddrs \
    -Wl,--wrap=etcp_socket_add -Wl,--wrap=stcp_server_listen -Wl,--wrap=random_bytes \
    -Wl,--wrap=bind -Wl,--wrap=socket_close_wrapper
test_local_transport_SOURCES = test_local_transport.c $(test_clock_sources)
test_local_transport_LDADD = $(LIBUTUN) $(COMMON_LIBS)
test_local_transport_LDFLAGS = $(test_clock_ldflags) -Wl,--wrap=local_interfaces -Wl,--wrap=socket_sendto
check_PROGRAMS += test_route_connectivity
test_route_connectivity_SOURCES = test_route_connectivity.c $(test_clock_sources)
test_route_connectivity_LDADD = $(LIBUTUN) $(COMMON_LIBS)
test_route_connectivity_LDFLAGS = $(test_clock_ldflags) -Wl,--wrap=etcp_send_ping_to_socket -Wl,--wrap=socket_sendto
check_PROGRAMS += test_local_routes
test_local_routes_SOURCES = test_local_routes.c $(test_clock_sources)
test_local_routes_LDADD = $(LIBUTUN) $(COMMON_LIBS)
test_local_routes_LDFLAGS = $(test_clock_ldflags) -Wl,--wrap=local_interfaces -Wl,--wrap=socket_sendto
check_PROGRAMS += test_local_transport_packets
test_local_transport_packets_SOURCES = test_local_transport_packets.c $(test_clock_sources)
test_local_transport_packets_LDADD = $(LIBUTUN) $(COMMON_LIBS)
test_local_transport_packets_LDFLAGS = $(test_clock_ldflags) -Wl,--wrap=local_interfaces -Wl,--wrap=socket_sendto \
    -Wl,--wrap=local_receive -Wl,--wrap=uasync_add_socket_t
test_stcp_LDFLAGS = -Wl,--wrap=send -Wl,--wrap=recv -Wl,--wrap=random_bytes -Wl,--wrap=u_malloc_impl -Wl,--wrap=u_calloc_impl
standby_test_sources = ../tools/chatgui-android/libutun_lite/standby.c ../src/utun_instance.c ../src/transport_layer/etcp_keepalive.c
standby_test_cppflags = $(AM_CPPFLAGS) -I$(top_srcdir)/src/chat -I$(top_srcdir)/tools/chatgui-android/libutun_lite -DUTUN_HAVE_STANDBY
test_standby_SOURCES = ../tools/chatgui-android/libutun_lite/tests/test_standby.c $(standby_test_sources) $(test_clock_sources)
test_standby_CPPFLAGS = $(standby_test_cppflags)
test_standby_LDADD = $(LIBUTUN) $(COMMON_LIBS)
test_standby_LDFLAGS = $(test_clock_ldflags)
test_standby_transport_SOURCES = test_standby_transport.c $(standby_test_sources) $(test_clock_sources)
test_standby_transport_CPPFLAGS = $(standby_test_cppflags)
test_standby_transport_LDADD = $(LIBUTUN) $(COMMON_LIBS)
test_standby_transport_LDFLAGS = $(test_clock_ldflags) -Wl,--wrap=etcp_encrypt_send -Wl,--wrap=uasync_set_timeout -Wl,--wrap=RAND_bytes
test_group_connect_standby_SOURCES = test_group_connect.c ../src/routing_layer/topo_group_connect.c $(standby_test_sources) $(test_clock_sources)
test_group_connect_standby_CPPFLAGS = $(standby_test_cppflags)
test_group_connect_standby_LDADD = $(LIBUTUN) $(COMMON_LIBS)
test_group_connect_standby_LDFLAGS = $(test_clock_ldflags)
test_android_udp_log_SOURCES = test_android_udp_log.c ../tools/chatgui-android/jni_bridge/android_udp_log.c
test_android_udp_log_LDADD = $(COMMON_LIBS)
test_android_udp_log_LDFLAGS = -Wl,--wrap=uasync_set_timeout -Wl,--wrap=uasync_cancel_timeout
android_bridge_test_sources = \
    ../tools/chatgui-android/jni_bridge/android_jni_bridge.c ../tools/chatgui-android/jni_bridge/android_udp_log.c \
    ../tools/chatgui-android/libutun_lite/instance_lite.c ../tools/chatgui-android/libutun_lite/utun_config_api.c \
    ../tools/chatgui-android/libutun_lite/attachment_sender.c ../tools/chatgui-android/libutun_lite/photo_sender.c \
    ../tools/chatgui-android/libutun_lite/video_sender.c ../tools/chatgui-android/libutun_lite/voice_recorder.c \
    ../tools/chatgui-android/libutun_lite/standby.c ../tools/chatgui-android/libutun_lite/stream_bridge.c
test_android_replies_SOURCES = test_android_replies.c $(android_bridge_test_sources)
test_android_replies_CPPFLAGS = $(AM_CPPFLAGS) -I$(top_srcdir)/src/chat -I$(top_srcdir)/src/media_async
test_android_replies_CFLAGS = $(AM_CFLAGS) -ffunction-sections -fdata-sections
test_android_replies_LDADD = $(LIBUTUN) $(COMMON_LIBS)
test_android_replies_LDFLAGS = -Wl,--gc-sections -Wl,--wrap=instance_lite_get_instance -Wl,--wrap=instance_lite_get_uasync \
    -Wl,--wrap=instance_lite_is_running -Wl,--wrap=instance_lite_post
check_PROGRAMS += test_android_bridge
test_android_bridge_SOURCES = test_android_bridge.c $(android_bridge_test_sources)
test_android_bridge_CPPFLAGS = $(test_android_replies_CPPFLAGS)
test_android_bridge_CFLAGS = $(test_android_replies_CFLAGS)
test_android_bridge_LDADD = $(LIBUTUN) $(COMMON_LIBS)
test_android_bridge_LDFLAGS = -Wl,--gc-sections
check_PROGRAMS += test_android_member_query
test_android_member_query_SOURCES = test_android_member_query.c $(android_bridge_test_sources)
test_android_member_query_CPPFLAGS = $(test_android_replies_CPPFLAGS)
test_android_member_query_CFLAGS = $(test_android_replies_CFLAGS)
test_android_member_query_LDADD = $(LIBUTUN) $(COMMON_LIBS)
test_android_member_query_LDFLAGS = -Wl,--gc-sections -Wl,--wrap=sqlite3_prepare_v2 -Wl,--wrap=sqlite3_exec -Wl,--wrap=u_calloc_impl
test_pool_corruption_log_SOURCES = test_pool_corruption_log.c
test_pool_corruption_log_LDADD = $(COMMON_LIBS)

# Linux/ELF: faults внедряются линкером, тестовых веток в библиотеках нет.
test_firewall_SOURCES = test_firewall.c
test_firewall_LDADD = $(LIBUTUN) $(CRYPTO_LIBS) $(COMMON_LIBS)
test_firewall_LDFLAGS = -Wl,--wrap=u_calloc_impl -Wl,--wrap=u_realloc_impl -Wl,--wrap=qsort -Wl,--wrap=etcp_route_send_key

test_mem_SOURCES = test_mem.c
test_mem_LDADD = $(COMMON_LIBS)
test_mem_LDFLAGS = -Wl,--wrap=malloc
test_mem_threads_SOURCES = test_mem_threads.c
test_mem_threads_LDADD = $(COMMON_LIBS)

test_config_updater_SOURCES = test_config_updater.c
test_config_updater_LDADD = $(LIBUTUN) $(COMMON_LIBS)
test_config_updater_LDFLAGS = -Wl,--wrap=fopen -Wl,--wrap=fdopen -Wl,--wrap=fread -Wl,--wrap=__fread_chk -Wl,--wrap=ferror \
    -Wl,--wrap=fwrite -Wl,--wrap=fflush -Wl,--wrap=fclose -Wl,--wrap=fsync -Wl,--wrap=fchmod -Wl,--wrap=rename -Wl,--wrap=open \
    -Wl,--wrap=u_malloc_impl -Wl,--wrap=u_calloc_impl -Wl,--wrap=u_realloc_impl -Wl,--wrap=u_strdup_impl

check_PROGRAMS += test_control_server
test_control_server_SOURCES = test_control_server.c
test_control_server_LDADD = $(LIBUTUN) $(COMMON_LIBS)
test_control_server_LDFLAGS = -Wl,--wrap=accept -Wl,--wrap=send -Wl,--wrap=uasync_add_socket_t \
    -Wl,--wrap=uasync_set_timeout -Wl,--wrap=u_malloc_impl
async_regression_sources = async_regression.c async_regression.h
async_regression_ldflags = -Wl,--wrap=u_malloc_impl -Wl,--wrap=u_calloc_impl -Wl,--wrap=u_realloc_impl \
    -Wl,--wrap=gettimeofday -Wl,--wrap=epoll_wait -Wl,--wrap=epoll_ctl -Wl,--wrap=eventfd -Wl,--wrap=epoll_create1
test_etcp_padding_SOURCES = test_etcp_padding.c
test_etcp_padding_LDADD = $(LIBUTUN) $(COMMON_LIBS)
test_etcp_padding_LDFLAGS = -Wl,--wrap=uasync_set_timeout -Wl,--wrap=socket_sendto
test_broadcast_cache_SOURCES = test_broadcast_cache.c
test_broadcast_cache_LDADD = $(LIBUTUN) $(COMMON_LIBS)
test_broadcast_cache_LDFLAGS = -Wl,--wrap=clock_gettime -Wl,--wrap=uasync_set_timeout
test_net_emulator_SOURCES = test_net_emulator.c $(test_clock_sources)
test_net_emulator_LDADD = $(COMMON_LIBS)
test_net_emulator_LDFLAGS = $(test_clock_ldflags) -Wl,--wrap=uasync_add_socket
test_uasync_regressions_SOURCES = test_uasync_regressions.c $(async_regression_sources)
test_uasync_regressions_LDADD = $(COMMON_LIBS)
test_uasync_regressions_LDFLAGS = $(async_regression_ldflags)
test_uasync_load_SOURCES = test_uasync_load.c
test_uasync_load_LDADD = $(COMMON_LIBS)
test_uasync_load_LDFLAGS = -Wl,--wrap=clock_gettime -Wl,--wrap=epoll_wait -Wl,--wrap=poll -Wl,--wrap=epoll_create1
test_ll_queue_regressions_SOURCES = test_ll_queue_regressions.c $(async_regression_sources)
test_ll_queue_regressions_LDADD = $(COMMON_LIBS)
test_ll_queue_regressions_LDFLAGS = $(async_regression_ldflags)
endif

# Test definitions
test_etcp_bbr_SOURCES = test_etcp_bbr.c
test_etcp_bbr_CFLAGS = -I$(top_srcdir)/src -I$(top_srcdir)/lib
test_etcp_bbr_LDADD = $(LIBUTUN) $(COMMON_LIBS)

test_etcp_crypto_SOURCES = test_etcp_crypto.c
test_crypto_pool_SOURCES = test_crypto_pool.c
test_crypto_pool_LDADD = $(LIBUTUN) $(COMMON_LIBS)
test_packet_transport_SOURCES = test_packet_transport.c
test_packet_transport_LDADD = $(LIBUTUN) $(COMMON_LIBS)
test_stream_transport_SOURCES = test_stream_transport.c
test_stream_transport_LDADD = $(LIBUTUN) $(COMMON_LIBS)
test_etcp_crypto_CFLAGS = -I$(top_srcdir)/src -I$(top_srcdir)/lib
test_etcp_crypto_LDADD = $(LIBUTUN) $(CRYPTO_LIBS) $(COMMON_LIBS)
test_etcp_crypto_LDFLAGS = -Wl,--wrap=RAND_bytes

test_stream_cipher_SOURCES = test_stream_cipher.c
test_stream_cipher_CFLAGS = -I$(top_srcdir)/src -I$(top_srcdir)/lib
test_stream_cipher_LDADD = $(LIBUTUN) $(CRYPTO_LIBS) $(COMMON_LIBS)

test_stream_sign_SOURCES = test_stream_sign.c
test_stream_sign_CFLAGS = -I$(top_srcdir)/src -I$(top_srcdir)/lib
test_stream_sign_LDADD = $(LIBUTUN) $(CRYPTO_LIBS) $(COMMON_LIBS)
if OS_LINUX
test_stream_sign_CFLAGS += -DSC_SIGN_FAULT_INJECTION
test_stream_sign_LDFLAGS = -Wl,--wrap=EVP_PKEY_new_raw_private_key -Wl,--wrap=EVP_PKEY_new_raw_public_key \
    -Wl,--wrap=EVP_MD_CTX_new -Wl,--wrap=EVP_DigestInit_ex -Wl,--wrap=EVP_DigestFinal_ex \
    -Wl,--wrap=EVP_DigestSignInit -Wl,--wrap=EVP_DigestSign -Wl,--wrap=EVP_DigestVerifyInit -Wl,--wrap=EVP_DigestVerify
endif

test_reality_hello_SOURCES = test_reality_hello.c
test_reality_hello_CFLAGS = -I$(top_srcdir)/src -I$(top_srcdir)/src/transport_layer -I$(top_srcdir)/lib
test_reality_hello_LDADD = $(LIBUTUN) $(CRYPTO_LIBS) $(COMMON_LIBS)

test_reality_bgp_SOURCES = test_reality_bgp.c
test_reality_bgp_CFLAGS = -I$(top_srcdir)/src -I$(top_srcdir)/src/transport_layer -I$(top_srcdir)/src/routing_layer -I$(top_srcdir)/lib
test_reality_bgp_LDADD = $(LIBUTUN) $(CRYPTO_LIBS) $(COMMON_LIBS)

test_reality_config_SOURCES = test_reality_config.c
test_reality_config_CFLAGS = -I$(top_srcdir)/src -I$(top_srcdir)/src/transport_layer -I$(top_srcdir)/lib
test_reality_config_LDADD = $(LIBUTUN) $(CRYPTO_LIBS) $(COMMON_LIBS)

test_dm_SOURCES = test_dm.c
test_attachment_SOURCES = test_attachment.c
test_attachment_LDADD = $(LIBUTUN) $(CRYPTO_LIBS) $(COMMON_LIBS)
if OS_LINUX
test_attachment_LDFLAGS = -Wl,--wrap=opus_codec_encoder_create
endif
test_encrypted_file_SOURCES = test_encrypted_file.c
test_encrypted_file_LDADD = $(LIBUTUN) $(CRYPTO_LIBS) $(COMMON_LIBS)
if OS_LINUX
test_encrypted_file_LDFLAGS = -Wl,--wrap=fopen -Wl,--wrap=fdopen -Wl,--wrap=fread -Wl,--wrap=__fread_chk \
    -Wl,--wrap=fwrite -Wl,--wrap=ferror -Wl,--wrap=fflush -Wl,--wrap=fclose -Wl,--wrap=fseeko \
    -Wl,--wrap=fsync -Wl,--wrap=rename -Wl,--wrap=random_bytes
endif
test_encrypted_attachment_SOURCES = test_encrypted_attachment.c $(test_clock_sources)
test_encrypted_attachment_CFLAGS = $(test_dm_e2e_CFLAGS)
test_encrypted_attachment_LDADD = $(LIBUTUN) $(CRYPTO_LIBS) $(COMMON_LIBS)
test_encrypted_attachment_LDFLAGS = $(test_clock_ldflags)
if OS_LINUX
test_encrypted_attachment_LDFLAGS += -Wl,--wrap=uasync_set_timeout -Wl,--wrap=media_download_handle_chunk \
    -Wl,--wrap=sqlite3_bind_blob -Wl,--wrap=etcp_router_conn_send -Wl,--wrap=etcp_router_bind
endif
if OS_LINUX
test_attachment_requests_SOURCES = test_attachment_requests.c
check_PROGRAMS += test_attachment_control_async
test_attachment_control_async_SOURCES = test_attachment_control_async.c
test_attachment_control_async_CFLAGS = $(test_core_db_async_CFLAGS)
test_attachment_control_async_LDADD = $(test_core_db_async_LDADD)
test_attachment_control_async_LDFLAGS = -Wl,--wrap=sqlite3_prepare_v2 -Wl,--wrap=sqlite3_exec -Wl,--wrap=u_calloc_impl \
    -Wl,--wrap=etcp_router_peer_reachable -Wl,--wrap=etcp_router_conn_get_key -Wl,--wrap=etcp_router_conn_send_q_has_room -Wl,--wrap=etcp_router_conn_send
check_PROGRAMS += test_attachment_file_async
test_attachment_file_async_SOURCES = test_attachment_file_async.c
test_attachment_file_async_CFLAGS = $(test_core_db_async_CFLAGS)
test_attachment_file_async_LDADD = $(test_core_db_async_LDADD)
test_attachment_file_async_LDFLAGS = -Wl,--wrap=sqlite3_prepare_v2 -Wl,--wrap=sqlite3_exec -Wl,--wrap=sqlite3_finalize \
    -Wl,--wrap=u_calloc_impl -Wl,--wrap=media_async_submit
test_attachment_requests_LDADD = $(LIBUTUN) $(CRYPTO_LIBS) $(COMMON_LIBS)
test_attachment_requests_LDFLAGS = -Wl,--wrap=sqlite3_exec -Wl,--wrap=sqlite3_prepare_v2 -Wl,--wrap=sqlite3_bind_blob \
    -Wl,--wrap=sqlite3_bind_text -Wl,--wrap=sqlite3_bind_int64 -Wl,--wrap=sqlite3_bind_int -Wl,--wrap=sqlite3_step \
    -Wl,--wrap=sqlite3_finalize -Wl,--wrap=sqlite3_changes -Wl,--wrap=etcp_router_peer_reachable -Wl,--wrap=etcp_router_conn_get_key \
    -Wl,--wrap=etcp_router_conn_send_q_has_room -Wl,--wrap=etcp_router_conn_send -Wl,--wrap=uasync_set_timeout
test_attachment_decisions_SOURCES = test_attachment_decisions.c
test_attachment_decisions_LDADD = $(LIBUTUN) $(CRYPTO_LIBS) $(COMMON_LIBS)
test_attachment_decisions_LDFLAGS = -Wl,--wrap=sqlite3_exec -Wl,--wrap=sqlite3_prepare_v2 -Wl,--wrap=sqlite3_bind_blob \
    -Wl,--wrap=sqlite3_bind_text -Wl,--wrap=sqlite3_bind_int64 -Wl,--wrap=sqlite3_bind_int -Wl,--wrap=sqlite3_step \
    -Wl,--wrap=sqlite3_finalize -Wl,--wrap=sqlite3_changes
test_attachment_results_SOURCES = test_attachment_results.c
test_attachment_results_LDADD = $(LIBUTUN) $(CRYPTO_LIBS) $(COMMON_LIBS)
test_attachment_results_LDFLAGS = -Wl,--wrap=sqlite3_exec -Wl,--wrap=sqlite3_prepare_v2 -Wl,--wrap=sqlite3_bind_blob \
    -Wl,--wrap=sqlite3_bind_text -Wl,--wrap=sqlite3_bind_int64 -Wl,--wrap=sqlite3_bind_int -Wl,--wrap=sqlite3_step \
    -Wl,--wrap=sqlite3_finalize -Wl,--wrap=sqlite3_changes -Wl,--wrap=etcp_router_peer_reachable -Wl,--wrap=etcp_router_conn_get_key \
    -Wl,--wrap=etcp_router_conn_send_q_has_room -Wl,--wrap=etcp_router_conn_send
test_chat_attachment_SOURCES = test_chat_attachment.c
test_chat_attachment_LDADD = $(LIBUTUN) $(CRYPTO_LIBS) $(COMMON_LIBS)
test_chat_attachment_LDFLAGS = -Wl,--wrap=sqlite3_exec -Wl,--wrap=sqlite3_prepare_v2 -Wl,--wrap=sqlite3_bind_blob \
    -Wl,--wrap=sqlite3_bind_text -Wl,--wrap=sqlite3_bind_int64 -Wl,--wrap=sqlite3_step -Wl,--wrap=sqlite3_finalize \
    -Wl,--wrap=media_async_submit -Wl,--wrap=sqlite3_changes -Wl,--wrap=uasync_set_timeout -Wl,--wrap=sqlite3_bind_int \
    -Wl,--wrap=media_download_start
endif
test_attachment_send_SOURCES = test_attachment_send.c
check_PROGRAMS += test_attachment_store
test_attachment_store_SOURCES = test_attachment_store.c
test_attachment_store_LDADD = $(LIBUTUN) $(CRYPTO_LIBS) $(COMMON_LIBS)
check_PROGRAMS += test_attachment_send_async
test_attachment_send_async_SOURCES = test_attachment_send_async.c
test_attachment_send_async_CFLAGS = $(test_core_db_async_CFLAGS)
test_attachment_send_async_LDADD = $(test_core_db_async_LDADD)
test_attachment_send_async_LDFLAGS = -Wl,--wrap=sqlite3_prepare_v2 -Wl,--wrap=sqlite3_exec -Wl,--wrap=u_calloc_impl \
    -Wl,--wrap=media_async_submit -Wl,--wrap=dm_send_file_trampoline
test_attachment_send_LDADD = $(LIBUTUN) $(CRYPTO_LIBS) $(COMMON_LIBS)
if OS_LINUX
test_attachment_send_LDFLAGS = -Wl,--wrap=media_async_submit
endif
test_dm_CFLAGS = -I$(top_srcdir)/src -I$(top_srcdir)/src/transport_layer -I$(top_srcdir)/lib
test_dm_LDADD = $(LIBUTUN) $(CRYPTO_LIBS) $(COMMON_LIBS)

test_transport_SOURCES = test_stcp_link.c $(test_clock_sources)
test_transport_CFLAGS = -I$(top_srcdir)/src -I$(top_srcdir)/lib
test_transport_LDADD = $(LIBUTUN) $(CRYPTO_LIBS) $(COMMON_LIBS)
test_transport_LDFLAGS = $(test_clock_ldflags)

test_etcp_transport_SOURCES = test_etcp_stcp.c $(test_clock_sources)
test_etcp_transport_CFLAGS = -I$(top_srcdir)/src -I$(top_srcdir)/lib
test_etcp_transport_LDADD = $(LIBUTUN) $(CRYPTO_LIBS) $(COMMON_LIBS)
test_etcp_transport_LDFLAGS = $(test_clock_ldflags)
if OS_LINUX
test_etcp_transport_LDFLAGS += -Wl,--wrap=etcp_encrypt_send
endif

test_stcp_SOURCES = test_stcp.c
test_stcp_CFLAGS = -I$(top_srcdir)/src -I$(top_srcdir)/lib
test_stcp_LDADD = $(LIBUTUN) $(CRYPTO_LIBS) $(COMMON_LIBS)

test_socks_client_SOURCES = test_socks_client.c $(test_clock_sources)
test_socks_client_CFLAGS = -I$(top_srcdir)/src -I$(top_srcdir)/src/transport_layer -I$(top_srcdir)/lib
test_socks_client_LDADD = $(LIBUTUN) $(CRYPTO_LIBS) $(COMMON_LIBS)
test_socks_client_LDFLAGS = $(test_clock_ldflags) -Wl,--wrap=uasync_set_timeout -Wl,--wrap=connect -Wl,--wrap=socket_send

test_etcp_two_instances_SOURCES = test_etcp_two_instances.c
test_etcp_two_instances_CFLAGS = -I$(top_srcdir)/src -I$(top_srcdir)/lib
test_etcp_two_instances_LDADD = $(LIBUTUN) $(CRYPTO_LIBS) $(COMMON_LIBS)

test_etcp_simple_traffic_SOURCES = test_etcp_simple_traffic.c
test_etcp_simple_traffic_CFLAGS = -I$(top_srcdir)/src -I$(top_srcdir)/lib
test_etcp_simple_traffic_LDADD = $(LIBUTUN) $(CRYPTO_LIBS) $(COMMON_LIBS)

test_ipv6_sockets_SOURCES = test_ipv6_sockets.c
test_ipv6_sockets_CFLAGS = -I$(top_srcdir)/src -I$(top_srcdir)/lib
test_ipv6_sockets_LDADD = $(LIBUTUN) $(CRYPTO_LIBS) $(COMMON_LIBS)

test_tcp_proxy_client_SOURCES = test_tcp_proxy_client.c
test_tcp_proxy_client_LDADD = $(LIBUTUN) $(CRYPTO_LIBS) $(COMMON_LIBS)

test_socks_http_proxy_SOURCES = test_socks_http_proxy.c
test_socks_http_proxy_LDADD = $(LIBUTUN) $(CRYPTO_LIBS) $(COMMON_LIBS)
test_socks_http_proxy_CPPFLAGS = $(AM_CPPFLAGS) -DPROXY_LOAD_SCRIPT='"$(abs_srcdir)/proxy_rolling_load.py"'

test_lwip_tcp_SOURCES = test_lwip_tcp.c
test_lwip_tcp_CFLAGS = -I$(top_srcdir)/src -I$(top_srcdir)/lib
test_lwip_tcp_LDADD = $(LIBUTUN) $(COMMON_LIBS)

test_etcp_router_SOURCES = test_etcp_router.c
test_etcp_router_LDADD = $(LIBUTUN) $(CRYPTO_LIBS) $(COMMON_LIBS)

test_etcp_router_faults_SOURCES = test_etcp_router_faults.c $(test_clock_sources)
test_etcp_router_faults_LDADD = $(LIBUTUN) $(CRYPTO_LIBS) $(COMMON_LIBS)
test_etcp_router_faults_LDFLAGS = $(test_clock_ldflags)

test_etcp_router_unit_SOURCES = test_etcp_router_unit.c
test_etcp_router_unit_LDADD = $(LIBUTUN) $(CRYPTO_LIBS) $(COMMON_LIBS)

test_multicast_SOURCES = test_multicast.c $(test_clock_sources)
test_multicast_LDADD = $(LIBUTUN) $(CRYPTO_LIBS) $(COMMON_LIBS)
test_multicast_LDFLAGS = $(test_clock_ldflags)

test_etcp_router_reconnect_SOURCES = test_etcp_router_reconnect.c
test_etcp_router_reconnect_LDADD = $(LIBUTUN) $(CRYPTO_LIBS) $(COMMON_LIBS)

test_tcp_proxy_server_SOURCES = test_tcp_proxy_server.c
test_tcp_proxy_server_CFLAGS = -I$(top_srcdir)/lib
test_tcp_proxy_server_LDADD = $(COMMON_LIBS)

test_udp_proxy_SOURCES = test_udp_proxy.c
test_udp_proxy_LDADD = $(LIBUTUN) $(CRYPTO_LIBS) $(COMMON_LIBS)

test_icmp_proxy_SOURCES = test_icmp_proxy.c
test_icmp_proxy_LDADD = $(LIBUTUN) $(CRYPTO_LIBS) $(COMMON_LIBS)

test_radix_SOURCES = test_radix.c
test_radix_CFLAGS = -I$(top_srcdir)/lib
test_radix_LDADD = $(COMMON_LIBS)

test_route6_lib_SOURCES = test_route6_lib.c
test_route6_lib_CFLAGS = -I$(top_srcdir)/src -I$(top_srcdir)/lib
test_route6_lib_LDADD = $(LIBUTUN) $(COMMON_LIBS)

test_etcp_minimal_LDADD = $(LIBUTUN) $(CRYPTO_LIBS) $(COMMON_LIBS)

test_etcp_100_packets_SOURCES = test_etcp_100_packets.c
test_etcp_100_packets_CFLAGS = -I$(top_srcdir)/src -I$(top_srcdir)/lib
test_etcp_100_packets_LDADD = $(LIBUTUN) $(CRYPTO_LIBS) $(COMMON_LIBS)

test_etcp_reconnect_SOURCES = test_etcp_reconnect.c
test_etcp_reconnect_CFLAGS = -I$(top_srcdir)/src -I$(top_srcdir)/lib
test_etcp_reconnect_LDADD = $(LIBUTUN) $(CRYPTO_LIBS) $(COMMON_LIBS)

test_etcp_seq_collision_SOURCES = test_etcp_seq_collision.c
test_etcp_seq_collision_CFLAGS = -I$(top_srcdir)/src -I$(top_srcdir)/lib
test_etcp_seq_collision_LDADD = $(LIBUTUN) $(CRYPTO_LIBS) $(COMMON_LIBS)

test_pkt_normalizer_etcp_SOURCES = test_pkt_normalizer_etcp.c
test_pkt_normalizer_etcp_CFLAGS = -I$(top_srcdir)/src -I$(top_srcdir)/lib
test_pkt_normalizer_etcp_LDADD = $(LIBUTUN) $(CRYPTO_LIBS) $(COMMON_LIBS)

test_pkt_normalizer_standalone_SOURCES = test_pkt_normalizer_standalone.c
test_pkt_normalizer_standalone_CFLAGS = -I$(top_srcdir)/src -I$(top_srcdir)/lib
test_pkt_normalizer_standalone_LDADD = $(LIBUTUN) $(CRYPTO_LIBS) $(COMMON_LIBS)

test_etcp_api_SOURCES = test_etcp_api.c
test_etcp_api_CFLAGS = -I$(top_srcdir)/src -I$(top_srcdir)/lib
test_etcp_api_LDADD = $(LIBUTUN) $(CRYPTO_LIBS) $(COMMON_LIBS)

test_etcp_ping_SOURCES = test_etcp_ping.c
test_etcp_ping_CFLAGS = -I$(top_srcdir)/src -I$(top_srcdir)/lib
test_etcp_ping_LDADD = $(LIBUTUN) $(CRYPTO_LIBS) $(COMMON_LIBS)

test_stcp_ping_SOURCES = test_stcp_ping.c
test_stcp_ping_CFLAGS = -I$(top_srcdir)/src -I$(top_srcdir)/lib
test_stcp_ping_LDADD = $(LIBUTUN) $(CRYPTO_LIBS) $(COMMON_LIBS)

test_route_ping_SOURCES = test_route_ping.c
test_route_ping_CFLAGS = -I$(top_srcdir)/src -I$(top_srcdir)/lib
test_route_ping_LDADD = $(LIBUTUN) $(CRYPTO_LIBS) $(COMMON_LIBS)

test_nat_detection_SOURCES = test_nat_detection.c $(test_clock_sources)
test_nat_detection_CFLAGS = -I$(top_srcdir)/src -I$(top_srcdir)/lib
test_nat_detection_LDADD = $(LIBUTUN) $(CRYPTO_LIBS) $(COMMON_LIBS)

if OS_LINUX
test_nat_detection_LDFLAGS = $(test_clock_ldflags) -Wl,--wrap=etcp_add_conn_status_cbk -Wl,--wrap=etcp_add_socket_cbk -Wl,--wrap=uasync_set_timeout
check_PROGRAMS += test_nat_detection_faults
test_nat_detection_faults_SOURCES = test_nat_detection_faults.c $(test_clock_sources)
test_nat_detection_faults_CFLAGS = -I$(top_srcdir)/src -I$(top_srcdir)/lib
test_nat_detection_faults_LDADD = $(LIBUTUN) $(CRYPTO_LIBS) $(COMMON_LIBS)
test_nat_detection_faults_LDFLAGS = $(test_clock_ldflags) -Wl,--wrap=socket_sendto \
    -Wl,--wrap=nat_detection_record_observation -Wl,--wrap=nat_detection_receive_ingress -Wl,--wrap=etcp_route_send_key
endif

test_stun_nat_SOURCES = test_stun_nat.c $(test_clock_sources)
test_stun_nat_CFLAGS = -I$(top_srcdir)/src -I$(top_srcdir)/lib
test_stun_nat_LDADD = $(LIBUTUN) $(CRYPTO_LIBS) $(COMMON_LIBS)
test_stun_nat_LDFLAGS = $(test_clock_ldflags) -Wl,--wrap=socket_create_udp

test_nat_engine_SOURCES = test_nat_engine.c
test_nat_engine_CFLAGS = -I$(top_srcdir)/src -I$(top_srcdir)/lib
test_nat_engine_LDADD = $(LIBUTUN) $(COMMON_LIBS)
if OS_LINUX
test_nat_engine_LDFLAGS = -Wl,--wrap=u_calloc_impl
endif

test_nat_transport_SOURCES = test_nat_transport.c
test_nat_transport_CFLAGS = -I$(top_srcdir)/src -I$(top_srcdir)/lib
test_nat_transport_LDADD = $(LIBUTUN) $(CRYPTO_LIBS) $(COMMON_LIBS)
if OS_LINUX
test_nat_transport_LDFLAGS = -Wl,--wrap=etcp_route_send_key -Wl,--wrap=queue_data_put -Wl,--wrap=tun_write \
    -Wl,--wrap=queue_dgram_free -Wl,--wrap=queue_entry_free
endif

test_nat_stress_SOURCES = test_nat_stress.c
test_nat_stress_CFLAGS = -I$(top_srcdir)/src -I$(top_srcdir)/lib
test_nat_stress_LDADD = $(LIBUTUN) $(COMMON_LIBS)

test_ll_queue_SOURCES = test_ll_queue.c
test_ll_queue_CFLAGS = -I$(top_srcdir)/src -I$(top_srcdir)/lib
test_ll_queue_LDADD = $(COMMON_LIBS)

test_serialize_SOURCES = test_serialize.c
test_serialize_CFLAGS = -I$(top_srcdir)/lib
test_serialize_LDADD = $(COMMON_LIBS)

test_reed_solomon_SOURCES = test_reed_solomon.c
test_reed_solomon_CFLAGS = -I$(top_srcdir)/lib
test_reed_solomon_LDADD = $(COMMON_LIBS)

test_stream_core_SOURCES = test_stream_core.c $(test_clock_sources)
test_stream_core_LDADD = $(LIBUTUN) $(COMMON_LIBS)
test_stream_core_LDFLAGS = $(test_clock_ldflags)

test_stream_control_expiry_SOURCES = test_stream_control_expiry.c
test_stream_control_expiry_LDADD = $(LIBUTUN) $(COMMON_LIBS)

test_stream_budget_SOURCES = test_stream_budget.c
test_stream_budget_LDADD = $(LIBUTUN) $(COMMON_LIBS)

test_stream_standby_SOURCES = test_stream_standby.c
test_stream_standby_LDADD = $(LIBUTUN) $(COMMON_LIBS)

test_stream_media_SOURCES = test_stream_media.c
test_stream_media_LDADD = $(LIBUTUN) $(COMMON_LIBS)

test_stream_playback_SOURCES = test_stream_playback.c
test_stream_playback_LDADD = $(LIBUTUN) $(COMMON_LIBS)
test_stream_tx_stats_SOURCES = test_stream_tx_stats.c
test_stream_tx_stats_LDADD = $(LIBUTUN) $(COMMON_LIBS)

test_android_stream_SOURCES = test_android_stream.c $(test_clock_sources) ../tools/chatgui-android/libutun_lite/stream_bridge.c
test_android_stream_LDADD = $(LIBUTUN) $(COMMON_LIBS)
test_android_stream_LDFLAGS = $(test_clock_ldflags)

test_stream_service_SOURCES = test_stream_service.c test_stream_failover.h test_stream_overload.h test_stream_paths.h test_stream_live.h $(test_clock_sources)
test_stream_service_LDADD = $(LIBUTUN) $(COMMON_LIBS)
test_stream_service_LDFLAGS = $(test_clock_ldflags) -Wl,--wrap=uasync_get_load \
    -Wl,--wrap=u_malloc_impl -Wl,--wrap=u_calloc_impl -Wl,--wrap=u_realloc_impl -Wl,--wrap=u_strdup_impl

test_stream_paths_SOURCES = $(test_stream_service_SOURCES)
test_stream_paths_CFLAGS = $(AM_CFLAGS) -DSTREAM_PATHS_TEST_ONLY
test_stream_paths_LDADD = $(test_stream_service_LDADD)
test_stream_paths_LDFLAGS = $(test_stream_service_LDFLAGS)

test_stream_live_SOURCES = $(test_stream_service_SOURCES)
test_stream_live_CFLAGS = $(AM_CFLAGS) -DSTREAM_LIVE_TEST_ONLY
test_stream_live_LDADD = $(test_stream_service_LDADD)
test_stream_live_LDFLAGS = $(test_stream_service_LDFLAGS)

# Та же реальная сетевая fixture, отдельный процесс для подписанного удаления/восстановления мемберов.
test_stream_member_SOURCES = $(test_stream_service_SOURCES)
test_stream_member_CFLAGS = $(AM_CFLAGS) -DSTREAM_MEMBER_TEST_ONLY
test_stream_member_LDADD = $(test_stream_service_LDADD)
test_stream_member_LDFLAGS = $(test_stream_service_LDFLAGS)

# Длительные BGP grace-периоды не увеличивают 90-секундный лимит остальных service-проверок.
test_stream_offline_SOURCES = $(test_stream_service_SOURCES)
test_stream_offline_CFLAGS = $(AM_CFLAGS) -DSTREAM_OFFLINE_TEST_ONLY
test_stream_offline_LDADD = $(test_stream_service_LDADD)
test_stream_offline_LDFLAGS = $(test_stream_service_LDFLAGS)

# Длинная гонка ретрансляторов изолирована от остальных сценариев и укладывается в общий timeout.
test_stream_relay_SOURCES = $(test_stream_service_SOURCES)
test_stream_relay_CFLAGS = $(AM_CFLAGS) -DSTREAM_RELAY_TEST_ONLY
test_stream_relay_LDADD = $(test_stream_service_LDADD)
test_stream_relay_LDFLAGS = $(test_stream_service_LDFLAGS)

test_stream_publisher_SOURCES = test_stream_publisher.c $(test_clock_sources)
test_stream_publisher_LDADD = $(LIBUTUN) $(COMMON_LIBS)
test_stream_publisher_LDFLAGS = $(test_clock_ldflags)
if OS_LINUX
test_stream_publisher_LDFLAGS += -Wl,--wrap=uasync_set_timeout -Wl,--wrap=uasync_get_load -Wl,--wrap=stream_stats_viewer_count
endif

test_intensive_memory_pool_SOURCES = test_intensive_memory_pool.c
test_intensive_memory_pool_CFLAGS = -I$(top_srcdir)/src -I$(top_srcdir)/lib
test_intensive_memory_pool_LDADD = $(COMMON_LIBS)

test_tcp_io_SOURCES = test_tcp_io.c
test_tcp_io_CFLAGS = -I$(top_srcdir)/lib
test_tcp_io_LDADD = $(COMMON_LIBS)
if OS_LINUX
test_tcp_io_LDFLAGS = -Wl,--wrap=uasync_set_timeout -Wl,--wrap=shutdown
endif

test_uasync_socket_race_SOURCES = test_uasync_socket_race.c
test_uasync_socket_race_CFLAGS = -I$(top_srcdir)/lib
test_uasync_socket_race_LDADD = $(COMMON_LIBS)

test_ntp_SOURCES = test_ntp.c
test_ntp_CFLAGS = -I$(top_srcdir)/src -I$(top_srcdir)/lib
test_ntp_LDADD = $(LIBUTUN) $(CRYPTO_LIBS) $(COMMON_LIBS)

if OS_LINUX
test_ntp_regressions_SOURCES = test_ntp_regressions.c
test_ntp_regressions_LDADD = $(LIBUTUN) $(CRYPTO_LIBS) $(COMMON_LIBS)
test_ntp_regressions_LDFLAGS = -Wl,--wrap=gettimeofday -Wl,--wrap=uasync_set_timeout -Wl,--wrap=uasync_cancel_timeout \
    -Wl,--wrap=socket_create_udp -Wl,--wrap=socket_set_nonblocking -Wl,--wrap=socket_close_wrapper \
    -Wl,--wrap=socket_sendto -Wl,--wrap=socket_recvfrom -Wl,--wrap=uasync_add_socket_t -Wl,--wrap=uasync_remove_socket_t \
    -Wl,--wrap=adns_resolve -Wl,--wrap=adns_cancel -Wl,--wrap=topo_node_update_my_addresses -Wl,--wrap=etcp_send \
    -Wl,--wrap=u_malloc_impl -Wl,--wrap=u_calloc_impl -Wl,--wrap=u_strdup_impl
endif

test_async_dns_SOURCES = test_async_dns.c
test_async_dns_CFLAGS = -I$(top_srcdir)/src -I$(top_srcdir)/lib
test_async_dns_LDADD = $(COMMON_LIBS)

test_memory_pool_and_config_SOURCES = test_memory_pool_and_config.c
test_memory_pool_and_config_CFLAGS = -I$(top_srcdir)/src -I$(top_srcdir)/lib
test_memory_pool_and_config_LDADD = $(COMMON_LIBS)

test_packet_dump_SOURCES = test_packet_dump.c ../src/transport_layer/packet_dump.c
test_packet_dump_CFLAGS = -I$(top_srcdir)/src -I$(top_srcdir)/lib
test_packet_dump_LDADD = $(COMMON_LIBS)

test_u_async_comprehensive_SOURCES = test_u_async_comprehensive.c
test_u_async_comprehensive_CFLAGS = -I$(top_srcdir)/src -I$(top_srcdir)/lib
test_u_async_comprehensive_LDADD = $(COMMON_LIBS)

test_u_async_performance_SOURCES = test_u_async_performance.c
test_u_async_performance_CFLAGS = -I$(top_srcdir)/src -I$(top_srcdir)/lib
test_u_async_performance_LDADD = $(COMMON_LIBS)

test_u_async_timeouts_SOURCES = test_u_async_timeouts.c
test_u_async_timeouts_CFLAGS = -I$(top_srcdir)/src -I$(top_srcdir)/lib
test_u_async_timeouts_LDADD = $(COMMON_LIBS)

test_debug_categories_SOURCES = test_debug_categories.c
test_debug_categories_CFLAGS = -I$(top_srcdir)/src -I$(top_srcdir)/lib
test_debug_categories_LDADD = $(COMMON_LIBS)

test_log_rotation_SOURCES = test_log_rotation.c
test_log_rotation_LDADD = $(COMMON_LIBS)

test_packet_link_SOURCES = test_packet_link.c
test_packet_link_LDADD = $(COMMON_LIBS)

test_etcp_packet_link_SOURCES = test_etcp_packet_link.c
test_etcp_packet_link_LDADD = $(LIBUTUN) $(COMMON_LIBS)

test_config_debug_SOURCES = test_config_debug.c
test_config_debug_CFLAGS = -I$(top_srcdir)/src -I$(top_srcdir)/lib
test_config_debug_LDADD = $(LIBUTUN) $(COMMON_LIBS)

test_route_lib_SOURCES = test_route_lib.c
test_route_lib_CFLAGS = -I$(top_srcdir)/src -I$(top_srcdir)/lib
test_route_lib_LDADD = $(LIBUTUN) $(COMMON_LIBS)

test_bgp_route_exchange_SOURCES = test_bgp_route_exchange.c test_connection_loss.h $(test_clock_sources)
test_bgp_route_exchange_CFLAGS = -I$(top_srcdir)/src -I$(top_srcdir)/lib
test_bgp_route_exchange_LDADD = $(LIBUTUN) $(CRYPTO_LIBS) $(COMMON_LIBS)
test_bgp_route_exchange_LDFLAGS = $(test_clock_ldflags)

test_bgp_triangle_SOURCES = test_bgp_triangle.c test_connection_loss.h $(test_clock_sources)
test_bgp_triangle_CFLAGS = -I$(top_srcdir)/src -I$(top_srcdir)/lib
test_bgp_triangle_LDADD = $(LIBUTUN) $(CRYPTO_LIBS) $(COMMON_LIBS)
test_bgp_triangle_LDFLAGS = $(test_clock_ldflags)

test_broadcast_SOURCES = test_broadcast.c
test_broadcast_CFLAGS = -I$(top_srcdir)/src -I$(top_srcdir)/lib
test_broadcast_LDADD = $(LIBUTUN) $(CRYPTO_LIBS) $(COMMON_LIBS)

test_chat_group_isolation_SOURCES = test_chat_group_isolation.c test_membership.h test_net.h
test_chat_group_isolation_CFLAGS = -I$(top_srcdir)/src -I$(top_srcdir)/lib
test_chat_group_isolation_LDADD = $(LIBUTUN) $(CRYPTO_LIBS) $(COMMON_LIBS)
test_group_api_isolation_SOURCES = test_group_api_isolation.c test_membership.h test_net.h
test_group_api_isolation_CFLAGS = $(test_chat_group_isolation_CFLAGS)
test_group_api_isolation_LDADD = $(test_chat_group_isolation_LDADD)
test_group_api_isolation_LDFLAGS = -Wl,--wrap=etcp_route_send_key
if OS_LINUX
test_chat_group_isolation_LDFLAGS = -Wl,--wrap=mf_storage_execute
test_group_api_isolation_LDFLAGS += $(test_chat_group_isolation_LDFLAGS)
endif

test_conn_mgr_SOURCES = test_conn_mgr.c
test_conn_mgr_CFLAGS = -I$(top_srcdir)/src -I$(top_srcdir)/lib
test_conn_mgr_LDADD = $(LIBUTUN) $(CRYPTO_LIBS) $(COMMON_LIBS)

test_conn_mgr_phases_SOURCES = test_conn_mgr_phases.c $(test_clock_sources)
test_conn_mgr_phases_CFLAGS = -I$(top_srcdir)/src -I$(top_srcdir)/lib
test_conn_mgr_phases_LDADD = $(LIBUTUN) $(CRYPTO_LIBS) $(COMMON_LIBS)
test_conn_mgr_phases_LDFLAGS = $(test_clock_ldflags)

test_conn_mgr_handles_SOURCES = test_conn_mgr_handles.c
test_conn_mgr_handles_CFLAGS = -I$(top_srcdir)/src -I$(top_srcdir)/lib
test_conn_mgr_handles_LDADD = $(LIBUTUN) $(CRYPTO_LIBS) $(COMMON_LIBS)
if OS_LINUX
test_conn_mgr_handles_LDFLAGS = -Wl,--wrap=uasync_call_soon
endif

test_conn_mgr_already_connected_SOURCES = test_conn_mgr_already_connected.c
test_conn_mgr_already_connected_CFLAGS = -I$(top_srcdir)/src -I$(top_srcdir)/lib
test_conn_mgr_already_connected_LDADD = $(LIBUTUN) $(CRYPTO_LIBS) $(COMMON_LIBS)

test_sock_match_SOURCES = test_sock_match.c
test_sock_match_CFLAGS = -I$(top_srcdir)/src -I$(top_srcdir)/src/transport_layer -I$(top_srcdir)/src/routing_layer -I$(top_srcdir)/lib
test_sock_match_LDADD = $(LIBUTUN) $(CRYPTO_LIBS) $(COMMON_LIBS)


test_etcp_connect_SOURCES = test_etcp_connect.c
test_etcp_connect_CFLAGS = -I$(top_srcdir)/src -I$(top_srcdir)/lib
test_etcp_connect_LDADD = $(LIBUTUN) $(CRYPTO_LIBS) $(COMMON_LIBS)

test_node_conn_direct_SOURCES = test_node_conn_direct.c
test_node_conn_direct_CFLAGS = -I$(top_srcdir)/src -I$(top_srcdir)/lib
test_node_conn_direct_LDADD = $(LIBUTUN) $(CRYPTO_LIBS) $(COMMON_LIBS)

if OS_LINUX
test_ncd_faults_SOURCES = test_ncd_faults.c $(test_clock_sources)
test_ncd_faults_CFLAGS = -I$(top_srcdir)/src -I$(top_srcdir)/lib
test_ncd_faults_LDADD = $(LIBUTUN) $(CRYPTO_LIBS) $(COMMON_LIBS)
test_ncd_faults_LDFLAGS = $(test_clock_ldflags) -Wl,--wrap=etcp_conn_add_cbk -Wl,--wrap=uasync_call_soon \
    -Wl,--wrap=uasync_set_timeout -Wl,--wrap=u_calloc_impl -Wl,--wrap=socket_sendto
endif

test_group_ownership_SOURCES = test_group_ownership.c
test_group_ownership_CFLAGS = -I$(top_srcdir)/src -I$(top_srcdir)/lib
test_group_ownership_LDADD = $(LIBUTUN) $(CRYPTO_LIBS) $(COMMON_LIBS)

test_group_exchange_SOURCES = test_group_exchange.c
test_group_exchange_CFLAGS = -I$(top_srcdir)/src -I$(top_srcdir)/lib
test_group_exchange_LDADD = $(LIBUTUN) $(CRYPTO_LIBS) $(COMMON_LIBS)
if OS_LINUX
test_group_exchange_LDFLAGS = -Wl,--wrap=queue_entry_new
endif

test_bgp_paths_SOURCES = test_bgp_paths.c router_wire_observer.h
test_bgp_paths_CFLAGS = -I$(top_srcdir)/src -I$(top_srcdir)/lib
test_bgp_paths_LDADD = $(LIBUTUN) $(CRYPTO_LIBS) $(COMMON_LIBS)

test_group_recovery_SOURCES = test_group_recovery.c $(test_clock_sources)
test_group_recovery_CFLAGS = -I$(top_srcdir)/src -I$(top_srcdir)/lib
test_group_recovery_LDADD = $(LIBUTUN) $(CRYPTO_LIBS) $(COMMON_LIBS)
test_group_recovery_LDFLAGS = $(test_clock_ldflags)

test_group_connect_SOURCES = test_group_connect.c $(test_clock_sources)
test_group_connect_CFLAGS = -I$(top_srcdir)/src -I$(top_srcdir)/lib
test_group_connect_LDADD = $(LIBUTUN) $(CRYPTO_LIBS) $(COMMON_LIBS)
test_group_connect_LDFLAGS = $(test_clock_ldflags)

test_node_rtt_SOURCES = test_node_rtt.c $(test_clock_sources)
test_node_rtt_CFLAGS = -I$(top_srcdir)/src -I$(top_srcdir)/lib
test_node_rtt_LDADD = $(LIBUTUN) $(CRYPTO_LIBS) $(COMMON_LIBS)
test_node_rtt_LDFLAGS = $(test_clock_ldflags)

test_node_snapshot_SOURCES = test_node_snapshot.c
test_node_snapshot_CFLAGS = -I$(top_srcdir)/src -I$(top_srcdir)/lib
test_node_snapshot_LDADD = $(LIBUTUN) $(CRYPTO_LIBS) $(COMMON_LIBS)

test_node_record_async_SOURCES = test_node_record_async.c
test_node_record_async_CFLAGS = -I$(top_srcdir)/src -I$(top_srcdir)/lib
test_node_record_async_LDADD = $(LIBUTUN) $(CRYPTO_LIBS) $(COMMON_LIBS)
if OS_LINUX
test_node_record_async_LDFLAGS = -Wl,--wrap=sqlite3_prepare_v2 -Wl,--wrap=memory_pool_alloc_impl \
    -Wl,--wrap=queue_data_put_with_index \
    -Wl,--wrap=sqlite3_bind_int64 -Wl,--wrap=sqlite3_finalize \
    -Wl,--wrap=u_malloc_impl -Wl,--wrap=u_calloc_impl -Wl,--wrap=u_realloc_impl -Wl,--wrap=u_strdup_impl
endif

test_core_db_async_SOURCES = test_core_db_async.c
test_core_db_async_CFLAGS = -I$(top_srcdir)/src -I$(top_srcdir)/lib
test_core_db_async_LDADD = $(LIBUTUN) $(CRYPTO_LIBS) $(COMMON_LIBS)
test_nat_history_async_SOURCES = test_nat_history_async.c
test_nat_history_async_CFLAGS = $(test_core_db_async_CFLAGS)
test_nat_history_async_LDADD = $(test_core_db_async_LDADD)
test_nat_history_async_LDFLAGS = -Wl,--wrap=sqlite3_prepare_v2 -Wl,--wrap=sqlite3_exec -Wl,--wrap=u_malloc_impl
test_auto_ports_async_SOURCES = test_auto_ports_async.c
test_auto_ports_async_CFLAGS = $(test_core_db_async_CFLAGS)
test_auto_ports_async_LDADD = $(test_core_db_async_LDADD)
test_auto_ports_async_LDFLAGS = -Wl,--wrap=sqlite3_prepare_v2 -Wl,--wrap=sqlite3_exec -Wl,--wrap=u_malloc_impl
test_ntp_offsets_async_SOURCES = test_ntp_offsets_async.c
test_ntp_offsets_async_CFLAGS = $(test_core_db_async_CFLAGS)
test_ntp_offsets_async_LDADD = $(test_core_db_async_LDADD)
test_ntp_offsets_async_LDFLAGS = -Wl,--wrap=sqlite3_prepare_v2 -Wl,--wrap=sqlite3_exec -Wl,--wrap=u_malloc_impl
test_stream_catalog_async_SOURCES = test_stream_catalog_async.c
test_stream_catalog_async_CFLAGS = $(test_core_db_async_CFLAGS)
test_stream_catalog_async_LDADD = $(test_core_db_async_LDADD)
test_stream_catalog_async_LDFLAGS = -Wl,--wrap=sqlite3_prepare_v2 -Wl,--wrap=sqlite3_exec -Wl,--wrap=u_calloc_impl

test_radio_mute_async_SOURCES = test_radio_mute_async.c
test_radio_mute_async_CFLAGS = $(test_core_db_async_CFLAGS)
test_radio_mute_async_LDADD = $(test_core_db_async_LDADD)
test_radio_mute_async_LDFLAGS = -Wl,--wrap=sqlite3_prepare_v2 -Wl,--wrap=sqlite3_exec -Wl,--wrap=u_calloc_impl \
    -Wl,--wrap=u_realloc_impl -Wl,--wrap=topo_group_set_radio

test_call_targets_async_SOURCES = test_call_targets_async.c
test_call_targets_async_CFLAGS = $(test_core_db_async_CFLAGS)
test_call_targets_async_LDADD = $(test_core_db_async_LDADD)
test_call_targets_async_LDFLAGS = -Wl,--wrap=sqlite3_prepare_v2 -Wl,--wrap=sqlite3_exec -Wl,--wrap=sqlite3_column_blob \
    -Wl,--wrap=sqlite3_finalize -Wl,--wrap=u_calloc_impl -Wl,--wrap=u_realloc_impl -Wl,--wrap=topo_group_find_conn_for_node \
    -Wl,--wrap=etcp_router_peer_reachable -Wl,--wrap=dm_call_route_submit

test_headless_db_async_SOURCES = test_headless_db_async.c
test_headless_db_async_CFLAGS = $(test_core_db_async_CFLAGS)
test_headless_db_async_LDADD = $(test_core_db_async_LDADD)
test_headless_db_async_LDFLAGS = -Wl,--wrap=sqlite3_prepare_v2 -Wl,--wrap=sqlite3_exec -Wl,--wrap=u_calloc_impl -Wl,--wrap=uasync_call_soon

test_dm_conversations_async_SOURCES = test_dm_conversations_async.c
test_dm_conversations_async_CFLAGS = $(test_core_db_async_CFLAGS)
test_dm_conversations_async_LDADD = $(test_core_db_async_LDADD)
test_dm_conversations_async_LDFLAGS = -Wl,--wrap=sqlite3_prepare_v2 -Wl,--wrap=sqlite3_exec -Wl,--wrap=sqlite3_step \
    -Wl,--wrap=sqlite3_finalize -Wl,--wrap=sqlite3_column_text -Wl,--wrap=u_calloc_impl -Wl,--wrap=u_realloc_impl

test_dm_receipt_async_SOURCES = test_dm_receipt_async.c
test_dm_receipt_async_CFLAGS = $(test_core_db_async_CFLAGS)
test_dm_receipt_async_LDADD = $(test_core_db_async_LDADD)
test_dm_receipt_async_LDFLAGS = -Wl,--wrap=sqlite3_prepare_v2 -Wl,--wrap=sqlite3_exec -Wl,--wrap=sqlite3_bind_int64 -Wl,--wrap=sqlite3_bind_blob \
    -Wl,--wrap=sqlite3_step -Wl,--wrap=sqlite3_finalize -Wl,--wrap=u_calloc_impl -Wl,--wrap=u_malloc_impl -Wl,--wrap=u_realloc_impl

test_dm_start_async_SOURCES = test_dm_start_async.c
test_dm_start_async_CFLAGS = $(test_core_db_async_CFLAGS)
test_dm_start_async_LDADD = $(test_core_db_async_LDADD)
dm_async_sqlite_wrap = -Wl,--wrap=sqlite3_prepare_v2 -Wl,--wrap=sqlite3_exec -Wl,--wrap=sqlite3_bind_int64 \
    -Wl,--wrap=sqlite3_step -Wl,--wrap=sqlite3_finalize -Wl,--wrap=u_calloc_impl
test_dm_start_async_LDFLAGS = $(dm_async_sqlite_wrap) -Wl,--wrap=sqlite3_bind_blob
test_dm_send_async_SOURCES = test_dm_send_async.c
test_dm_send_async_CFLAGS = $(test_core_db_async_CFLAGS)
test_dm_send_async_LDADD = $(test_core_db_async_LDADD)
test_dm_send_async_LDFLAGS = $(dm_async_sqlite_wrap) -Wl,--wrap=sqlite3_bind_text -Wl,--wrap=sqlite3_bind_blob \
    -Wl,--wrap=u_malloc_impl -Wl,--wrap=media_encrypt_stream
test_dm_receive_async_SOURCES = test_dm_receive_async.c
test_dm_receive_async_CFLAGS = $(test_core_db_async_CFLAGS)
test_dm_receive_async_LDADD = $(test_core_db_async_LDADD)
test_dm_receive_async_LDFLAGS = $(dm_async_sqlite_wrap) -Wl,--wrap=sqlite3_bind_text -Wl,--wrap=sqlite3_bind_blob \
    -Wl,--wrap=sc_ed25519_sign -Wl,--wrap=topo_group_chat_allowed -Wl,--wrap=etcp_router_conn_get \
    -Wl,--wrap=etcp_router_conn_send_q_has_room -Wl,--wrap=etcp_route_send -Wl,--wrap=etcp_router_unbind -Wl,--wrap=etcp_router_conn_send \
    -Wl,--wrap=topo_group_chat_key_allowed -Wl,--wrap=etcp_router_conn_get_key -Wl,--wrap=etcp_route_send_key
test_dm_download_async_SOURCES = test_dm_download_async.c
test_dm_teardown_async_SOURCES = test_dm_teardown_async.c
test_dm_teardown_async_CFLAGS = $(test_core_db_async_CFLAGS)
test_dm_teardown_async_LDADD = $(test_core_db_async_LDADD)
test_dm_teardown_async_LDFLAGS = -Wl,--wrap=sqlite3_prepare_v2 -Wl,--wrap=sqlite3_exec -Wl,--wrap=sqlite3_bind_blob \
    -Wl,--wrap=sqlite3_bind_text -Wl,--wrap=sqlite3_step -Wl,--wrap=sqlite3_finalize
check_PROGRAMS += test_media_index_async
test_media_index_async_SOURCES = test_media_index_async.c
test_media_index_async_CFLAGS = $(test_core_db_async_CFLAGS)
test_media_index_async_LDADD = $(test_core_db_async_LDADD)
test_media_index_async_LDFLAGS = -Wl,--wrap=sqlite3_prepare_v2 -Wl,--wrap=sqlite3_exec -Wl,--wrap=sqlite3_bind_text \
    -Wl,--wrap=sqlite3_step -Wl,--wrap=sqlite3_finalize -Wl,--wrap=u_calloc_impl -Wl,--wrap=file_io_sync_directory
check_PROGRAMS += test_media_delivery_async
test_media_delivery_async_SOURCES = test_media_delivery_async.c
test_media_delivery_async_CFLAGS = $(test_core_db_async_CFLAGS)
test_media_delivery_async_LDADD = $(test_core_db_async_LDADD)
test_media_delivery_async_LDFLAGS = -Wl,--wrap=sqlite3_prepare_v2 -Wl,--wrap=sqlite3_exec -Wl,--wrap=u_calloc_impl
check_PROGRAMS += test_member_files_db_async
test_member_files_db_async_SOURCES = test_member_files_db_async.c
test_member_files_db_async_CFLAGS = $(test_core_db_async_CFLAGS)
test_member_files_db_async_LDADD = $(test_core_db_async_LDADD)
test_member_files_db_async_LDFLAGS = -Wl,--wrap=sqlite3_prepare_v2 -Wl,--wrap=sqlite3_exec \
    -Wl,--wrap=topo_group_chat_key_allowed -Wl,--wrap=group_policy_member_role_key
check_PROGRAMS += test_media_download_db_async
test_media_download_db_async_SOURCES = test_media_download_db_async.c
test_media_download_db_async_CFLAGS = $(test_core_db_async_CFLAGS)
test_media_download_db_async_LDADD = $(test_core_db_async_LDADD)
test_media_download_db_async_LDFLAGS = -Wl,--wrap=sqlite3_prepare_v2 -Wl,--wrap=sqlite3_exec -Wl,--wrap=u_calloc_impl \
    -Wl,--wrap=topo_group_chat_allowed -Wl,--wrap=topo_group_chat_key_allowed -Wl,--wrap=group_policy_member_keys -Wl,--wrap=group_policy_member_verified -Wl,--wrap=md_send_key \
    -Wl,--wrap=media_delivery_send_have_block -Wl,--wrap=media_delivery_send_processing
check_PROGRAMS += test_file_transfer_lookup_async
test_file_transfer_lookup_async_SOURCES = test_file_transfer_lookup_async.c
test_file_transfer_lookup_async_CFLAGS = $(test_core_db_async_CFLAGS)
test_file_transfer_lookup_async_LDADD = $(test_core_db_async_LDADD)
test_file_transfer_lookup_async_LDFLAGS = -Wl,--wrap=etcp_router_conn_get_key -Wl,--wrap=etcp_router_conn_send_q_has_room -Wl,--wrap=group_policy_member_keys \
    -Wl,--wrap=etcp_router_conn_send -Wl,--wrap=etcp_router_set_max_inflight -Wl,--wrap=conn_mgr_open -Wl,--wrap=u_calloc_impl
test_dm_download_async_CFLAGS = $(test_core_db_async_CFLAGS)
test_dm_download_async_LDADD = $(test_core_db_async_LDADD)
test_dm_download_async_LDFLAGS = $(dm_async_sqlite_wrap) -Wl,--wrap=sqlite3_bind_text -Wl,--wrap=sqlite3_bind_blob \
    -Wl,--wrap=sc_ed25519_sign -Wl,--wrap=u_malloc_impl -Wl,--wrap=u_realloc_impl -Wl,--wrap=file_io_publish \
    -Wl,--wrap=file_transfer_pull -Wl,--wrap=file_transfer_destroy -Wl,--wrap=file_transfer_cancel_path \
    -Wl,--wrap=etcp_router_conn_get -Wl,--wrap=etcp_router_conn_get_key -Wl,--wrap=etcp_router_conn_send -Wl,--wrap=etcp_router_conn_send_q_has_room \
    -Wl,--wrap=etcp_router_unbind -Wl,--wrap=topo_group_chat_allowed -Wl,--wrap=topo_group_chat_key_allowed -Wl,--wrap=group_policy_member_keys
test_dm_outbox_async_SOURCES = test_dm_outbox_async.c
test_dm_outbox_async_CFLAGS = $(test_core_db_async_CFLAGS)
test_dm_outbox_async_LDADD = $(test_core_db_async_LDADD)
test_dm_outbox_async_LDFLAGS = $(dm_async_sqlite_wrap) -Wl,--wrap=u_malloc_impl -Wl,--wrap=ntp_time_get_seconds \
    -Wl,--wrap=topo_group_chat_allowed -Wl,--wrap=group_policy_role_key_next -Wl,--wrap=etcp_router_get_path \
    -Wl,--wrap=etcp_router_conn_get -Wl,--wrap=etcp_router_conn_send_q_has_room -Wl,--wrap=etcp_route_send \
    -Wl,--wrap=etcp_router_bind -Wl,--wrap=etcp_router_unbind -Wl,--wrap=etcp_router_require_crypto -Wl,--wrap=etcp_router_set_authorize \
    -Wl,--wrap=topo_group_chat_key_allowed -Wl,--wrap=etcp_router_peer_reachable -Wl,--wrap=etcp_router_conn_get_key -Wl,--wrap=etcp_route_send_key
if OS_LINUX
test_core_db_async_LDFLAGS = -Wl,--wrap=sqlite3_prepare_v2 -Wl,--wrap=sqlite3_finalize \
    -Wl,--wrap=sqlite3_exec -Wl,--wrap=sqlite3_get_table
endif

test_group_access_async_SOURCES = test_group_access_async.c
test_group_access_async_CFLAGS = $(test_core_db_async_CFLAGS)
test_group_access_async_LDADD = $(test_core_db_async_LDADD)
if OS_LINUX
test_group_access_async_LDFLAGS = -Wl,--wrap=sqlite3_prepare_v2 -Wl,--wrap=sqlite3_step -Wl,--wrap=sqlite3_finalize \
    -Wl,--wrap=u_calloc_impl -Wl,--wrap=u_realloc_impl
endif

test_kanban_read_async_SOURCES = test_kanban_read_async.c
test_kanban_read_async_CFLAGS = $(test_core_db_async_CFLAGS)
test_kanban_read_async_LDADD = $(test_core_db_async_LDADD)
if OS_LINUX
test_kanban_read_async_LDFLAGS = -Wl,--wrap=sqlite3_prepare_v2 -Wl,--wrap=sqlite3_step -Wl,--wrap=sqlite3_finalize \
    -Wl,--wrap=u_malloc_impl -Wl,--wrap=u_calloc_impl
endif

test_kanban_write_async_SOURCES = test_kanban_write_async.c
test_kanban_write_async_CFLAGS = $(test_core_db_async_CFLAGS)
test_kanban_write_async_LDADD = $(test_core_db_async_LDADD)
if OS_LINUX
test_kanban_write_async_LDFLAGS = -Wl,--wrap=sqlite3_prepare_v2 -Wl,--wrap=sqlite3_step -Wl,--wrap=sqlite3_finalize \
    -Wl,--wrap=sqlite3_exec -Wl,--wrap=u_realloc_impl
endif

test_member_read_async_SOURCES = test_member_read_async.c
test_member_read_async_CFLAGS = $(test_core_db_async_CFLAGS)
test_member_read_async_LDADD = $(test_core_db_async_LDADD)
if OS_LINUX
test_member_read_async_LDFLAGS = -Wl,--wrap=sqlite3_prepare_v2 -Wl,--wrap=sqlite3_bind_int64 -Wl,--wrap=sqlite3_bind_blob -Wl,--wrap=sqlite3_step \
    -Wl,--wrap=sqlite3_finalize -Wl,--wrap=u_realloc_impl -Wl,--wrap=u_calloc_impl -Wl,--wrap=u_malloc_impl
endif

test_group_info_read_async_SOURCES = test_group_info_read_async.c
test_group_info_read_async_CFLAGS = $(test_core_db_async_CFLAGS)
test_group_info_read_async_LDADD = $(test_core_db_async_LDADD)
if OS_LINUX
test_group_info_read_async_LDFLAGS = -Wl,--wrap=sqlite3_prepare_v2 -Wl,--wrap=sqlite3_bind_text -Wl,--wrap=sqlite3_step \
    -Wl,--wrap=sqlite3_finalize -Wl,--wrap=u_realloc_impl -Wl,--wrap=u_calloc_impl
endif

test_group_policy_read_async_SOURCES = test_group_policy_read_async.c
test_group_policy_read_async_CFLAGS = $(test_core_db_async_CFLAGS)
test_group_policy_read_async_LDADD = $(test_core_db_async_LDADD)
if OS_LINUX
test_group_policy_read_async_LDFLAGS = -Wl,--wrap=sqlite3_prepare_v2 -Wl,--wrap=sqlite3_bind_text -Wl,--wrap=sqlite3_step \
    -Wl,--wrap=sqlite3_finalize -Wl,--wrap=u_realloc_impl -Wl,--wrap=u_malloc_impl -Wl,--wrap=u_calloc_impl
endif

test_topo_node_wire_SOURCES = test_topo_node_wire.c
test_topo_node_wire_LDADD = $(LIBUTUN) $(CRYPTO_LIBS) $(COMMON_LIBS)
if OS_LINUX
test_topo_node_wire_LDFLAGS = -Wl,--wrap=u_malloc_impl -Wl,--wrap=u_calloc_impl -Wl,--wrap=u_realloc_impl -Wl,--wrap=memory_pool_alloc_impl \
    -Wl,--wrap=u_strdup_impl -Wl,--wrap=memory_pool_init -Wl,--wrap=topo_node_sqlite_init -Wl,--wrap=queue_entry_new -Wl,--wrap=uasync_set_timeout
endif

test_ncd_config_SOURCES = test_ncd_config.c
test_ncd_config_CFLAGS = -I$(top_srcdir)/src -I$(top_srcdir)/lib
test_ncd_config_LDADD = $(LIBUTUN) $(CRYPTO_LIBS) $(COMMON_LIBS)

test_db_sync_SOURCES = test_db_sync.c
test_db_sync_CFLAGS = -I$(top_srcdir)/src -I$(top_srcdir)/lib
test_db_sync_LDADD = $(LIBUTUN) $(CRYPTO_LIBS) $(COMMON_LIBS)

test_db_sync_schema_SOURCES = test_db_sync_schema.c
test_db_sync_schema_CFLAGS = -I$(top_srcdir)/src -I$(top_srcdir)/lib
test_db_sync_schema_LDADD = $(LIBUTUN) $(CRYPTO_LIBS) $(COMMON_LIBS)

if OS_LINUX
test_chat_admin_SOURCES = test_chat_admin.c
test_chat_admin_CFLAGS = -I$(top_srcdir)/src -I$(top_srcdir)/lib
test_chat_admin_LDADD = $(LIBUTUN) $(CRYPTO_LIBS) $(COMMON_LIBS)
test_chat_admin_LDFLAGS = -Wl,--wrap=sqlite3_exec -Wl,--wrap=sqlite3_prepare_v2 -Wl,--wrap=sqlite3_bind_blob \
    -Wl,--wrap=sqlite3_bind_text -Wl,--wrap=sqlite3_step -Wl,--wrap=sqlite3_finalize -Wl,--wrap=EVP_PKEY_new_raw_private_key

test_db_sync_integrity_SOURCES = test_db_sync_integrity.c
test_db_sync_integrity_CFLAGS = -I$(top_srcdir)/src -I$(top_srcdir)/lib
test_db_sync_integrity_LDADD = $(LIBUTUN) $(CRYPTO_LIBS) $(COMMON_LIBS)
test_db_sync_integrity_LDFLAGS = -Wl,--wrap=EVP_DigestVerifyInit -Wl,--wrap=sqlite3_exec -Wl,--wrap=uasync_set_timeout \
    -Wl,--wrap=sqlite3_column_blob -Wl,--wrap=sqlite3_errcode -Wl,--wrap=db_sync_message_id
endif

test_merkle_sync_SOURCES = test_merkle_sync.c $(test_clock_sources)
test_chat_feedback_SOURCES = test_chat_feedback.c
test_chat_feedback_CFLAGS = -I$(top_srcdir)/src -I$(top_srcdir)/src/chat -I$(top_srcdir)/lib
test_chat_feedback_LDADD = $(LIBUTUN) $(CRYPTO_LIBS) $(COMMON_LIBS)
test_chat_feedback_LDFLAGS = -Wl,--wrap=sqlite3_exec -Wl,--wrap=sqlite3_prepare_v2 -Wl,--wrap=sqlite3_bind_blob \
    -Wl,--wrap=sqlite3_bind_int64 -Wl,--wrap=sqlite3_bind_int -Wl,--wrap=sqlite3_step -Wl,--wrap=sqlite3_changes \
    -Wl,--wrap=sqlite3_finalize
test_merkle_protocol_SOURCES = test_merkle_protocol.c
test_merkle_protocol_CFLAGS = -I$(top_srcdir)/src -I$(top_srcdir)/src/chat -I$(top_srcdir)/lib
test_merkle_protocol_LDADD = $(LIBUTUN) $(CRYPTO_LIBS) $(COMMON_LIBS)
check_PROGRAMS += test_merkle_async
test_merkle_async_SOURCES = test_merkle_async.c
test_merkle_async_CFLAGS = $(test_merkle_protocol_CFLAGS)
test_merkle_async_LDADD = $(test_merkle_protocol_LDADD)
test_merkle_async_LDFLAGS = -Wl,--wrap=sqlite3_prepare_v2 -Wl,--wrap=sqlite3_exec -Wl,--wrap=u_calloc_impl
test_member_versions_LDFLAGS = -Wl,--wrap=sc_derive_node_id_from_pubkey
test_member_versions_SOURCES = test_member_versions.c
test_member_versions_CFLAGS = -I$(top_srcdir)/src -I$(top_srcdir)/src/chat -I$(top_srcdir)/lib
test_member_versions_LDADD = $(LIBUTUN) $(CRYPTO_LIBS) $(COMMON_LIBS)
test_chat_sync_schedule_SOURCES = test_chat_sync_schedule.c $(test_clock_sources)
test_chat_sync_schedule_CFLAGS = -I$(top_srcdir)/src -I$(top_srcdir)/src/chat -I$(top_srcdir)/lib
test_chat_sync_schedule_LDFLAGS = $(test_clock_ldflags)
test_chat_sync_schedule_LDADD = $(LIBUTUN) $(CRYPTO_LIBS) $(COMMON_LIBS)
test_member_adapter_SOURCES = test_member_adapter.c test_member_purge.c
test_group_policy_SOURCES = test_group_policy.c
test_group_policy_CFLAGS = -I$(top_srcdir)/src -I$(top_srcdir)/src/chat -I$(top_srcdir)/lib
test_group_policy_LDADD = $(LIBUTUN) $(CRYPTO_LIBS) $(COMMON_LIBS)
test_group_policy_LDFLAGS = -Wl,--wrap=sqlite3_prepare_v2 -Wl,--wrap=sqlite3_exec -Wl,--wrap=sqlite3_finalize -Wl,--wrap=json_flat_parse
test_member_adapter_CFLAGS = -I$(top_srcdir)/src -I$(top_srcdir)/src/chat -I$(top_srcdir)/lib
test_member_adapter_LDADD = $(LIBUTUN) $(CRYPTO_LIBS) $(COMMON_LIBS)
test_member_adapter_LDFLAGS = -Wl,--wrap=EVP_PKEY_new_raw_public_key -Wl,--wrap=EVP_MD_CTX_new \
    -Wl,--wrap=EVP_DigestVerifyInit -Wl,--wrap=EVP_DigestVerify -Wl,--wrap=SHA256 -Wl,--wrap=u_realloc_impl \
    -Wl,--wrap=sqlite3_prepare_v2 -Wl,--wrap=sqlite3_bind_blob -Wl,--wrap=sqlite3_bind_int64 -Wl,--wrap=sqlite3_step \
    -Wl,--wrap=sqlite3_finalize -Wl,--wrap=sqlite3_exec
test_merkle_sync_CFLAGS = -I$(top_srcdir)/src -I$(top_srcdir)/src/chat -I$(top_srcdir)/lib
test_merkle_sync_LDFLAGS = $(test_clock_ldflags)
test_merkle_sync_LDADD = $(LIBUTUN) $(CRYPTO_LIBS) $(COMMON_LIBS)

test_chat_sync_stress_SOURCES = test_chat_sync_stress.c
test_chat_sync_stress_CFLAGS = -I$(top_srcdir)/src -I$(top_srcdir)/lib
test_chat_sync_stress_LDADD = $(LIBUTUN) $(CRYPTO_LIBS) $(COMMON_LIBS)

test_chat_join_SOURCES = test_chat_join.c $(test_clock_sources)
test_chat_join_CFLAGS = -I$(top_srcdir)/src -I$(top_srcdir)/src/chat -I$(top_srcdir)/src/routing_layer -I$(top_srcdir)/src/transport_layer -I$(top_srcdir)/lib
test_chat_join_LDADD = $(LIBUTUN) $(CRYPTO_LIBS) $(COMMON_LIBS)
test_chat_join_LDFLAGS = $(test_clock_ldflags)

test_chat_join_e2e_SOURCES = test_chat_join_e2e.c
test_chat_join_e2e_CFLAGS = -I$(top_srcdir)/src -I$(top_srcdir)/src/chat -I$(top_srcdir)/src/routing_layer -I$(top_srcdir)/src/transport_layer -I$(top_srcdir)/lib
test_chat_join_e2e_LDADD = $(LIBUTUN) $(CRYPTO_LIBS) $(COMMON_LIBS)

test_dm_e2e_SOURCES = test_dm_e2e.c $(test_clock_sources)
test_dm_e2e_CFLAGS = -I$(top_srcdir)/src -I$(top_srcdir)/src/chat -I$(top_srcdir)/src/dm -I$(top_srcdir)/src/routing_layer -I$(top_srcdir)/src/transport_layer -I$(top_srcdir)/lib
test_dm_e2e_LDADD = $(LIBUTUN) $(CRYPTO_LIBS) $(COMMON_LIBS)
test_dm_e2e_LDFLAGS = $(test_clock_ldflags)

check_PROGRAMS += test_dm_group_routes
test_dm_group_routes_SOURCES = test_dm_group_routes.c test_membership.h test_net.h
test_dm_group_routes_CFLAGS = $(test_dm_e2e_CFLAGS)
test_dm_group_routes_LDADD = $(LIBUTUN) $(CRYPTO_LIBS) $(COMMON_LIBS)
test_dm_group_routes_LDFLAGS = -Wl,--wrap=sqlite3_prepare_v2

test_dm_invite_SOURCES = test_dm_invite.c $(test_clock_sources)
test_dm_invite_CFLAGS = $(test_dm_e2e_CFLAGS)
test_dm_invite_LDADD = $(LIBUTUN) $(CRYPTO_LIBS) $(COMMON_LIBS)
test_dm_invite_LDFLAGS = $(test_clock_ldflags)

test_dm_media_SOURCES = test_dm_media.c $(test_clock_sources)
test_dm_media_CFLAGS = $(test_dm_e2e_CFLAGS)
test_dm_media_LDADD = $(LIBUTUN) $(CRYPTO_LIBS) $(COMMON_LIBS)
test_dm_media_LDFLAGS = $(test_clock_ldflags)
if OS_LINUX
test_dm_media_CFLAGS += -DDM_QUOTA_FAULT_INJECTION
test_dm_media_LDFLAGS += -Wl,--wrap=sqlite3_prepare_v2 -Wl,--wrap=sqlite3_step \
    -Wl,--wrap=sqlite3_bind_blob -Wl,--wrap=sqlite3_finalize -Wl,--wrap=sqlite3_exec -Wl,--wrap=etcp_router_conn_send
check_PROGRAMS += test_dm_media_cleanup
test_dm_media_cleanup_SOURCES = test_dm_media_cleanup.c $(test_clock_sources)
test_dm_media_cleanup_CFLAGS = $(test_dm_e2e_CFLAGS)
test_dm_media_cleanup_LDADD = $(LIBUTUN) $(CRYPTO_LIBS) $(COMMON_LIBS)
test_dm_media_cleanup_LDFLAGS = $(test_clock_ldflags) -Wl,--wrap=sqlite3_prepare_v2 -Wl,--wrap=sqlite3_step \
    -Wl,--wrap=sqlite3_bind_blob -Wl,--wrap=sqlite3_bind_int64 -Wl,--wrap=sqlite3_finalize \
    -Wl,--wrap=dm_media_send_key -Wl,--wrap=dm_route_group_key
endif

test_dm_worker_SOURCES = test_dm_worker.c
test_dm_worker_CFLAGS = $(test_dm_e2e_CFLAGS)
test_dm_worker_LDADD = $(LIBUTUN) $(CRYPTO_LIBS) $(COMMON_LIBS)

test_dm_history_SOURCES = test_dm_history.c
test_dm_history_CFLAGS = $(test_dm_e2e_CFLAGS)
test_dm_history_LDADD = $(LIBUTUN) $(CRYPTO_LIBS) $(COMMON_LIBS)

test_dm_names_SOURCES = test_dm_names.c
test_dm_names_CFLAGS = $(test_dm_e2e_CFLAGS)
test_dm_names_LDADD = $(LIBUTUN) $(CRYPTO_LIBS) $(COMMON_LIBS)

if OS_LINUX
test_dm_load_SOURCES = test_dm_load.c
test_dm_load_CFLAGS = $(test_dm_e2e_CFLAGS)
test_dm_load_LDADD = $(LIBUTUN) $(CRYPTO_LIBS) $(COMMON_LIBS)
test_dm_load_LDFLAGS = -Wl,--wrap=sqlite3_prepare_v2 -Wl,--wrap=sqlite3_bind_text \
    -Wl,--wrap=sqlite3_step -Wl,--wrap=sqlite3_finalize -Wl,--wrap=EVP_PKEY_derive
endif

test_call_SOURCES = test_call.c $(test_clock_sources)
test_call_LDFLAGS = $(test_clock_ldflags)
test_route_tx_stats_SOURCES = test_route_tx_stats.c
test_route_tx_stats_LDADD = $(LIBUTUN) $(COMMON_LIBS)

test_call_audio_batch_SOURCES = test_call_audio_batch.c
test_call_audio_batch_LDADD = $(LIBUTUN) $(COMMON_LIBS)

test_call_video_rate_SOURCES = test_call_video_rate.c
test_call_video_rate_LDADD = $(LIBUTUN) $(COMMON_LIBS)

test_call_video_playout_SOURCES = test_call_video_playout.c
test_call_video_playout_LDADD = $(LIBUTUN) $(COMMON_LIBS)

test_call_video_port_SOURCES = test_call_video_port.c
test_call_video_port_LDADD = $(LIBUTUN) $(CRYPTO_LIBS) $(COMMON_LIBS)

test_call_video_SOURCES = test_call_video.c
test_call_video_LDADD = $(LIBUTUN) $(COMMON_LIBS)

test_video_SOURCES = test_video.c
test_video_LDADD = $(LIBUTUN) $(COMMON_LIBS)
test_video_no_ffmpeg_SOURCES = test_video_no_ffmpeg.c
test_video_no_ffmpeg_LDADD = $(COMMON_LIBS)
if HAVE_FFMPEG
if OS_LINUX
test_video_LDFLAGS = -Wl,--wrap=fdopen -Wl,--wrap=fwrite -Wl,--wrap=fflush -Wl,--wrap=fclose -Wl,--wrap=rename \
    -Wl,--wrap=av_write_trailer -Wl,--wrap=avcodec_send_packet -Wl,--wrap=avcodec_send_frame \
    -Wl,--wrap=avcodec_receive_packet -Wl,--wrap=avcodec_find_encoder -Wl,--wrap=pthread_create -Wl,--wrap=u_calloc_impl
endif
endif

test_video_plane_SOURCES = test_video_plane.c
test_video_plane_LDADD = ../lib/libuasync.a $(COMMON_LIBS)

test_call_video_flush_SOURCES = test_call_video_flush.c
test_call_video_flush_LDADD = $(LIBUTUN) $(CRYPTO_LIBS) $(COMMON_LIBS)
test_call_CFLAGS = -I$(top_srcdir)/src -I$(top_srcdir)/src/chat -I$(top_srcdir)/src/dm -I$(top_srcdir)/src/call -I$(top_srcdir)/src/routing_layer -I$(top_srcdir)/src/transport_layer -I$(top_srcdir)/lib
test_call_LDADD = $(LIBUTUN) $(CRYPTO_LIBS) $(COMMON_LIBS)

test_call_admission_SOURCES = test_call_admission.c $(test_clock_sources)
test_call_admission_CFLAGS = $(test_call_CFLAGS)
test_call_admission_LDADD = $(test_call_LDADD)
test_call_admission_LDFLAGS = $(test_clock_ldflags) -Wl,--wrap=etcp_route_send_key -Wl,--wrap=conn_mgr_open

test_call_headless_SOURCES = test_call_headless.c
test_call_headless_CFLAGS = -I$(top_srcdir)/src -I$(top_srcdir)/src/chat -I$(top_srcdir)/src/dm -I$(top_srcdir)/src/call -I$(top_srcdir)/src/routing_layer -I$(top_srcdir)/src/transport_layer -I$(top_srcdir)/lib
test_call_headless_LDADD = $(LIBUTUN) $(CRYPTO_LIBS) $(COMMON_LIBS)
if !OS_WINDOWS
check_PROGRAMS += test_call_headless_queue
test_call_headless_queue_SOURCES = test_call_headless_queue.c
test_call_headless_queue_CFLAGS = $(test_call_headless_CFLAGS)
test_call_headless_queue_LDADD = $(test_call_headless_LDADD)
test_call_headless_queue_LDFLAGS = -Wl,--wrap=send
endif

test_radio_SOURCES = test_radio.c $(test_clock_sources)
test_radio_CFLAGS = -I$(top_srcdir)/src -I$(top_srcdir)/src/chat -I$(top_srcdir)/src/dm -I$(top_srcdir)/src/call -I$(top_srcdir)/src/radio -I$(top_srcdir)/src/routing_layer -I$(top_srcdir)/src/transport_layer -I$(top_srcdir)/lib
test_radio_LDADD = $(LIBUTUN) $(CRYPTO_LIBS) $(COMMON_LIBS)
test_radio_LDFLAGS = $(test_clock_ldflags)

test_radio_batch_SOURCES = test_radio_batch.c
test_radio_batch_CFLAGS = $(test_radio_CFLAGS)
test_radio_batch_LDADD = $(test_radio_LDADD)

test_radio_headless_SOURCES = test_radio_headless.c
test_radio_headless_CFLAGS = -I$(top_srcdir)/src -I$(top_srcdir)/src/chat -I$(top_srcdir)/src/dm -I$(top_srcdir)/src/call -I$(top_srcdir)/src/radio -I$(top_srcdir)/src/routing_layer -I$(top_srcdir)/src/transport_layer -I$(top_srcdir)/lib
test_radio_headless_LDADD = $(LIBUTUN) $(CRYPTO_LIBS) $(COMMON_LIBS)

test_radio_mesh_SOURCES = test_radio_mesh.c
test_radio_mesh_CFLAGS = -I$(top_srcdir)/src -I$(top_srcdir)/src/chat -I$(top_srcdir)/src/dm -I$(top_srcdir)/src/call -I$(top_srcdir)/src/radio -I$(top_srcdir)/src/routing_layer -I$(top_srcdir)/src/transport_layer -I$(top_srcdir)/lib
test_radio_mesh_LDADD = $(LIBUTUN) $(CRYPTO_LIBS) $(COMMON_LIBS)
if !OS_WINDOWS
test_radio_mesh_CFLAGS += -DUTUN_TEST_MESH_OBSERVE
test_radio_mesh_LDFLAGS = -Wl,--wrap=etcp_bind -Wl,--wrap=etcp_send
endif

test_stcp_traffic_SOURCES = test_stcp_traffic.c
test_stcp_traffic_CFLAGS = -I$(top_srcdir)/src -I$(top_srcdir)/lib
test_stcp_traffic_LDADD = $(LIBUTUN) $(CRYPTO_LIBS) $(COMMON_LIBS)

test_bbr_integration_SOURCES = bbr_integration/test_bbr_integration.c
test_bbr_integration_CFLAGS = -I$(top_srcdir)/src -I$(top_srcdir)/lib
test_bbr_integration_LDADD = $(LIBUTUN) $(CRYPTO_LIBS) $(COMMON_LIBS)

bench_timeout_heap_SOURCES = bench_timeout_heap.c
bench_timeout_heap_CFLAGS = -I$(top_srcdir)/lib
bench_timeout_heap_LDADD = $(COMMON_LIBS)

bench_uasync_timeouts_SOURCES = bench_uasync_timeouts.c
bench_uasync_timeouts_CFLAGS = -I$(top_srcdir)/lib
bench_uasync_timeouts_LDADD = $(COMMON_LIBS)

test_opus_codec_SOURCES = test_opus_codec.c
test_opus_codec_CFLAGS = -I$(top_srcdir)/lib -I$(top_srcdir)/lib/libopus/include
test_opus_codec_LDADD = $(COMMON_LIBS) $(top_builddir)/lib/libopus/libopus_internal.a -lm

test_audio_compressor_SOURCES = test_audio_compressor.c
test_audio_compressor_LDADD = $(COMMON_LIBS) -lm

test_radio_vad_SOURCES = test_radio_vad.c
test_radio_vad_LDADD = -lm

test_silero_vad_SOURCES = test_silero_vad.c
test_silero_vad_CFLAGS = -I$(top_srcdir)/lib -DTEST_SILERO_SPEECH_PATH='"$(srcdir)/data/silero_speech.pcm"'
test_silero_vad_LDADD = $(COMMON_LIBS) @SILERO_VAD_LIBS@

test_speex_aec_SOURCES = test_speex_aec.c
test_speex_aec_CFLAGS = -I$(top_srcdir)/lib
test_speex_aec_LDADD = $(COMMON_LIBS)

test_speex_ns_SOURCES = test_speex_ns.c
test_speex_ns_LDADD = $(COMMON_LIBS)

test_media_async_SOURCES = test_media_async.c
test_media_async_CFLAGS = -I$(top_srcdir)/src -I$(top_srcdir)/src/media_async -I$(top_srcdir)/lib
test_media_async_LDADD = $(LIBUTUN) $(CRYPTO_LIBS) $(COMMON_LIBS)

check_PROGRAMS += test_media_primitives
test_media_primitives_SOURCES = test_media_primitives.c
test_media_primitives_LDADD = $(LIBUTUN) $(COMMON_LIBS)
if OS_LINUX
test_media_primitives_LDFLAGS = -Wl,--wrap=fopen -Wl,--wrap=fdopen -Wl,--wrap=fread -Wl,--wrap=__fread_chk -Wl,--wrap=ferror \
    -Wl,--wrap=fwrite -Wl,--wrap=fflush -Wl,--wrap=fclose -Wl,--wrap=rename -Wl,--wrap=RAND_bytes \
    -Wl,--wrap=opendir -Wl,--wrap=readdir -Wl,--wrap=closedir -Wl,--wrap=fseeko \
    -Wl,--wrap=pthread_create -Wl,--wrap=u_calloc_impl -Wl,--wrap=uasync_post_reserved -Wl,--wrap=fsync
endif

test_media_index_SOURCES = test_media_index.c
test_media_index_CFLAGS = -I$(top_srcdir)/src -I$(top_srcdir)/src/media_delivery -I$(top_srcdir)/src/media_async -I$(top_srcdir)/lib
test_media_index_LDADD = $(LIBUTUN) $(CRYPTO_LIBS) $(COMMON_LIBS)

check_PROGRAMS += test_media_index_regressions
test_media_index_regressions_SOURCES = test_media_index_regressions.c
test_media_index_regressions_LDADD = $(LIBUTUN) $(COMMON_LIBS)
if OS_LINUX
test_media_index_regressions_LDFLAGS = -Wl,--wrap=ma_sha256_file
endif

test_media_delivery_sql_SOURCES = test_media_delivery_sql.c media_delivery_fixture.h
test_media_delivery_sql_CFLAGS = -I$(top_srcdir)/src -I$(top_srcdir)/src/media_delivery -I$(top_srcdir)/lib
test_media_delivery_sql_LDADD = $(LIBUTUN) $(CRYPTO_LIBS) $(COMMON_LIBS)
test_media_delivery_sql_LDFLAGS = -Wl,--wrap=etcp_route_send -Wl,--wrap=etcp_route_send_key -Wl,--wrap=etcp_router_conn_send -Wl,--wrap=RAND_bytes
if OS_LINUX
test_media_delivery_sql_LDFLAGS += -Wl,--wrap=sqlite3_prepare_v2 -Wl,--wrap=sqlite3_bind_blob \
    -Wl,--wrap=sqlite3_step -Wl,--wrap=sqlite3_finalize
endif

test_media_delivery_download_SOURCES = test_media_delivery_download.c
test_media_delivery_download_CFLAGS = -I$(top_srcdir)/src -I$(top_srcdir)/src/media_delivery -I$(top_srcdir)/src/media_async -I$(top_srcdir)/lib -I$(top_srcdir)/src/transport_layer -I$(top_srcdir)/src/routing_layer -I$(top_srcdir)/src/chat
test_media_delivery_download_LDADD = $(LIBUTUN) $(CRYPTO_LIBS) $(COMMON_LIBS)
test_media_delivery_download_LDFLAGS = -Wl,--wrap=etcp_route_send_key

test_media_download_timeout_SOURCES = test_media_download_timeout.c
test_media_download_timeout_CFLAGS = -I$(top_srcdir)/src -I$(top_srcdir)/src/media_delivery -I$(top_srcdir)/src/media_async -I$(top_srcdir)/lib -I$(top_srcdir)/src/transport_layer -I$(top_srcdir)/src/routing_layer -I$(top_srcdir)/src/chat
test_media_download_timeout_LDADD = $(LIBUTUN) $(CRYPTO_LIBS) $(COMMON_LIBS)
test_media_download_timeout_LDFLAGS = -Wl,--wrap=etcp_route_send_key

check_PROGRAMS += test_media_query
test_media_query_SOURCES = test_media_query.c media_delivery_fixture.h
test_media_query_CFLAGS = $(test_media_delivery_download_CFLAGS)
test_media_query_LDADD = $(LIBUTUN) $(CRYPTO_LIBS) $(COMMON_LIBS)
test_media_query_LDFLAGS = -Wl,--wrap=etcp_route_send -Wl,--wrap=etcp_route_send_key -Wl,--wrap=RAND_bytes

check_PROGRAMS += test_media_replication
test_media_replication_SOURCES = test_media_replication.c media_delivery_fixture.h
test_media_replication_CFLAGS = $(test_media_delivery_download_CFLAGS)
test_media_replication_LDADD = $(LIBUTUN) $(CRYPTO_LIBS) $(COMMON_LIBS)
test_media_replication_LDFLAGS = -Wl,--wrap=etcp_route_send -Wl,--wrap=etcp_route_send_key -Wl,--wrap=RAND_bytes \
    -Wl,--wrap=conn_mgr_open -Wl,--wrap=conn_mgr_close

if OS_LINUX
check_PROGRAMS += test_media_blocks
test_media_blocks_SOURCES = test_media_blocks.c media_delivery_fixture.h
test_media_blocks_CFLAGS = $(test_media_delivery_download_CFLAGS)
test_media_blocks_LDADD = $(LIBUTUN) $(CRYPTO_LIBS) $(COMMON_LIBS)
test_media_blocks_LDFLAGS = -Wl,--wrap=etcp_route_send -Wl,--wrap=etcp_route_send_key -Wl,--wrap=etcp_router_conn_send -Wl,--wrap=RAND_bytes \
    -Wl,--wrap=fwrite -Wl,--wrap=fread -Wl,--wrap=__fread_chk \
    -Wl,--wrap=fseeko -Wl,--wrap=fclose -Wl,--wrap=fflush -Wl,--wrap=rename -Wl,--wrap=ma_sha256_file
endif

test_media_delivery_integration_SOURCES = test_media_delivery_integration.c
test_media_delivery_integration_CFLAGS = -I$(top_srcdir)/src -I$(top_srcdir)/src/media_delivery -I$(top_srcdir)/src/media_async -I$(top_srcdir)/lib -I$(top_srcdir)/src/transport_layer -I$(top_srcdir)/src/routing_layer -I$(top_srcdir)/src/chat
test_media_delivery_integration_LDADD = $(LIBUTUN) $(CRYPTO_LIBS) $(COMMON_LIBS)

test_etcp_link_stress_SOURCES = test_etcp_link_stress.c
test_etcp_link_stress_CFLAGS = -I$(top_srcdir)/src -I$(top_srcdir)/lib
test_etcp_link_stress_LDADD = $(LIBUTUN) $(CRYPTO_LIBS) $(COMMON_LIBS)

test_auto_socket_dynamic_SOURCES = test_auto_socket_dynamic.c $(test_clock_sources)
test_auto_socket_dynamic_CFLAGS = -I$(top_srcdir)/src -I$(top_srcdir)/src/transport_layer -I$(top_srcdir)/src/routing_layer -I$(top_srcdir)/lib
test_auto_socket_dynamic_LDADD = $(LIBUTUN) $(CRYPTO_LIBS) $(COMMON_LIBS)
test_auto_socket_dynamic_LDFLAGS = $(test_clock_ldflags)

test_media_delivery_full_SOURCES = test_media_delivery_full.c
test_media_delivery_full_CFLAGS = -I$(top_srcdir)/src -I$(top_srcdir)/src/media_delivery -I$(top_srcdir)/src/media_async -I$(top_srcdir)/lib -I$(top_srcdir)/src/transport_layer -I$(top_srcdir)/src/routing_layer -I$(top_srcdir)/src/chat
test_media_delivery_full_LDADD = $(LIBUTUN) $(CRYPTO_LIBS) $(COMMON_LIBS)

test_media_delivery_chat_SOURCES = test_media_delivery_chat.c
test_media_delivery_chat_CFLAGS = -I$(top_srcdir)/src -I$(top_srcdir)/src/media_delivery -I$(top_srcdir)/src/media_async -I$(top_srcdir)/lib -I$(top_srcdir)/src/transport_layer -I$(top_srcdir)/src/routing_layer -I$(top_srcdir)/src/chat
test_media_delivery_chat_LDADD = $(LIBUTUN) $(CRYPTO_LIBS) $(COMMON_LIBS)

# Copy test configs to build directory (tests run from build/tests/)
.PHONY: check-build copy-test-configs
check-build: $(check_PROGRAMS) copy-test-configs
	@echo "[INFO] test programs and configurations are ready"

copy-test-configs:
	@if test "$(abs_srcdir)" != "$(abs_builddir)"; then \
		for conf in $(srcdir)/test_*.conf $(srcdir)/test_*.json; do \
			if test -f "$$conf"; then \
				cp -f "$$conf" "$(abs_builddir)/" || { echo "ERROR: cannot copy $$conf" >&2; exit 1; }; \
			fi; \
		done; \
	fi

# Clean
clean-local:
	-rm -f $(check_PROGRAMS)
	@if test "$(abs_srcdir)" != "$(abs_builddir)"; then rm -f "$(abs_builddir)"/test_*.conf "$(abs_builddir)"/test_*.json; fi
	-rm -rf $(top_builddir)/tests/logs 2>/dev/null || true

# Test logs directory
TEST_LOG_DIR = $(top_builddir)/tests/logs

# По умолчанию jobs = доступные CPU; TEST_JOBS=1 сохраняет последовательный запуск.
check-local: check-runner check-service-ids check-build
	@python3 "$(srcdir)/run_tests.py" --log-dir "$(TEST_LOG_DIR)" $(check_PROGRAMS)

.PHONY: check-runner
check-runner:
	@python3 "$(srcdir)/test_run_tests.py"
	@echo "[INFO] test runner regression checks passed"

.PHONY: check-service-ids
check-service-ids:
	@python3 "$(srcdir)/test_service_ids.py"

.PHONY: check-build-script
check-build-script:
	@if command -v python3 >/dev/null 2>&1; then \
		python3 "$(srcdir)/test_build_script.py" "$(top_srcdir)/build.sh"; \
	else echo "[SKIP] build.sh regression checks require python3"; fi

.PHONY: check-windows-bootstrap
check-windows-bootstrap:
	@if command -v pwsh >/dev/null 2>&1; then \
		pwsh -NoLogo -NoProfile -File "$(srcdir)/test_windows_bootstrap.ps1" -Bootstrap "$(top_srcdir)/tools/build_windows.ps1"; \
	elif command -v powershell.exe >/dev/null 2>&1; then \
		powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "$(srcdir)/test_windows_bootstrap.ps1" \
			-Bootstrap "$(top_srcdir)/tools/build_windows.ps1"; \
	else echo "[SKIP] Windows bootstrap regression checks require PowerShell"; fi


# Интеграционный тест tcp_proxy_full (требует sudo, TUN, iptables). Запускается из check.sh.
.PHONY: check-proxy
check-proxy:
	@cd $(srcdir)/tcp_proxy_full && bash run_test.sh

# Burst-тест tcp_proxy: двусторонний byte-exact трафик со случайными idle (требует sudo).
.PHONY: check-burst
check-burst:
	@cd $(srcdir)/tcp_proxy_full && bash run_burst_test.sh

# Нагрузочный тест tcp_proxy: N параллельных соединений, byte-exact, замер пропускной (требует sudo).
.PHONY: check-load
check-load:
	@cd $(srcdir)/tcp_proxy_full && bash run_load_test.sh

# Полная RTT/BWlim/fault-матрица дольше 90s общего runner; отдельный запуск не скрывает её результат.
.PHONY: check-multipath
if OS_LINUX
check-multipath: test_dummynet_channels test_etcp_multipath
	./test_dummynet_channels
	./test_etcp_multipath
endif

test_etcp_lifecycle_SOURCES = test_etcp_lifecycle.c
test_etcp_lifecycle_LDADD = $(LIBUTUN) $(CRYPTO_LIBS) $(COMMON_LIBS)

test_etcp_server_lifecycle_SOURCES = test_etcp_server_lifecycle.c $(test_clock_sources)
test_etcp_server_lifecycle_LDADD = $(LIBUTUN) $(COMMON_LIBS)
test_etcp_server_lifecycle_LDFLAGS = $(test_clock_ldflags)

test_topo_recovery_SOURCES = test_topo_recovery.c
test_topo_recovery_LDADD = $(LIBUTUN) $(CRYPTO_LIBS) $(COMMON_LIBS)

test_etcp_session_SOURCES = test_etcp_session.c $(test_clock_sources)
test_etcp_session_LDADD = $(LIBUTUN) $(COMMON_LIBS)
test_etcp_session_LDFLAGS = $(test_clock_ldflags) -Wl,--wrap=ll_alloc_lldgram

test_etcp_init_liveness_SOURCES = test_etcp_init_liveness.c $(test_clock_sources)
test_etcp_init_liveness_LDADD = $(LIBUTUN) $(COMMON_LIBS)
test_etcp_init_liveness_LDFLAGS = $(test_clock_ldflags)

test_etcp_windows_SOURCES = test_etcp_windows.c $(test_clock_sources)
test_dummynet_channels_SOURCES = test_dummynet_channels.c $(test_clock_sources)
test_dummynet_channels_LDADD = $(LIBUTUN) $(COMMON_LIBS)
test_dummynet_channels_LDFLAGS = $(test_clock_ldflags) -Wl,--wrap=uasync_set_timeout -Wl,--wrap=queue_entry_new_from_pool \
    -Wl,--wrap=u_malloc_impl -Wl,--wrap=socket_sendto -Wl,--wrap=u_calloc_impl -Wl,--wrap=memory_pool_init \
    -Wl,--wrap=queue_new -Wl,--wrap=uasync_add_socket_t
test_etcp_multipath_SOURCES = test_etcp_multipath.c $(test_clock_sources)
test_etcp_multipath_LDADD = $(LIBUTUN) $(COMMON_LIBS)
test_etcp_multipath_LDFLAGS = $(test_clock_ldflags) -Wl,--wrap=etcp_conn_input -Wl,--wrap=bbr_main \
    -Wl,--wrap=etcp_tx_complete -Wl,--wrap=queue_data_put_with_index -Wl,--wrap=etcp_loadbalancer_select_link
test_stcp_sigpipe_SOURCES = test_stcp_sigpipe.c
test_stcp_sigpipe_LDADD = $(LIBUTUN) $(COMMON_LIBS)
test_etcp_windows_LDADD = $(LIBUTUN) $(COMMON_LIBS)
test_etcp_windows_LDFLAGS = $(test_clock_ldflags) -Wl,--wrap=get_time_tb -Wl,--wrap=get_time_us \
    -Wl,--wrap=etcp_encrypt_send -Wl,--wrap=memory_pool_alloc_impl \
    -Wl,--wrap=bbr_main -Wl,--wrap=uasync_set_timeout -Wl,--wrap=uasync_call_soon -Wl,--wrap=ll_alloc_lldgram \
    -Wl,--wrap=u_malloc_impl -Wl,--wrap=u_calloc_impl -Wl,--wrap=u_realloc_impl -Wl,--wrap=random_bytes \
    -Wl,--wrap=topo_node_ping_update_rtt

test_etcp_router_tcp_SOURCES = test_etcp_router_tcp.c
test_etcp_router_tcp_LDADD = $(LIBUTUN) $(COMMON_LIBS)

test_services_SOURCES = test_services.c
test_services_CFLAGS = -I$(top_srcdir)/src -I$(top_srcdir)/src/routing_layer -I$(top_srcdir)/src/transport_layer -I$(top_srcdir)/lib
test_services_LDADD = $(LIBUTUN) $(CRYPTO_LIBS) $(COMMON_LIBS)

test_tun_route_SOURCES = test_tun_route.c
test_tun_route_LDADD = $(LIBUTUN) $(COMMON_LIBS)
test_tun_route_LDFLAGS = -Wl,--wrap=sendto -Wl,--wrap=recvfrom -Wl,--wrap=__recvfrom_chk \
    -Wl,--wrap=poll -Wl,--wrap=__poll_chk -Wl,--wrap=tun_platform_init \
    -Wl,--wrap=socket -Wl,--wrap=bind

test_member_files_SOURCES = test_member_files.c
test_member_files_CFLAGS = $(test_services_CFLAGS)
test_member_files_LDADD = $(LIBUTUN) $(CRYPTO_LIBS) $(COMMON_LIBS)
if OS_LINUX
test_member_files_LDFLAGS = -Wl,--wrap=uasync_post_reserved
endif
test_member_files_network_SOURCES = test_member_files_network.c $(test_clock_sources)
test_member_files_network_CFLAGS = $(test_services_CFLAGS)
test_member_files_network_LDADD = $(LIBUTUN) $(CRYPTO_LIBS) $(COMMON_LIBS)
test_member_files_network_LDFLAGS = $(test_clock_ldflags)
test_group_avatar_SOURCES = test_group_avatar.c
test_group_avatar_CFLAGS = $(test_services_CFLAGS)
test_group_avatar_LDADD = $(LIBUTUN) $(CRYPTO_LIBS) $(COMMON_LIBS)
test_daemon_worker_SOURCES = test_daemon_worker.c
test_daemon_worker_CFLAGS = $(test_services_CFLAGS) -DUTUN_CLI_PATH='"$(abs_top_builddir)/src/utun"'
test_daemon_worker_LDADD = $(LIBUTUN) $(CRYPTO_LIBS) $(COMMON_LIBS)
test_daemon_worker_DEPENDENCIES = $(LIBUTUN) $(top_builddir)/src/utun

$(top_builddir)/src/utun:
	$(MAKE) -C $(top_builddir)/src utun

# Граничные длины и checksum UDP/ICMP после восстановления пакета.
test_proxy_packets_SOURCES = test_proxy_packets.c
test_proxy_packets_LDADD = $(LIBUTUN) $(CRYPTO_LIBS) $(COMMON_LIBS)

# Управляемый транспорт для граничных состояний proxy.
test_proxy_regressions_SOURCES = test_proxy_regressions.c $(test_clock_sources)
test_proxy_regressions_LDADD = $(LIBUTUN) $(CRYPTO_LIBS) $(COMMON_LIBS)
test_proxy_regressions_LDFLAGS = $(test_clock_ldflags) -Wl,--wrap=etcp_route_send -Wl,--wrap=etcp_route_send_key -Wl,--wrap=uasync_set_timeout \
    -Wl,--wrap=etcp_router_conn_lookup -Wl,--wrap=etcp_router_peer_reachable -Wl,--wrap=etcp_router_conn_on_send_ready -Wl,--wrap=group_policy_public_key -Wl,--wrap=topo_group_chat_key_allowed

test_http_proxy_SOURCES = test_http_proxy.c ../src/proxy/http_proxy.c
test_http_proxy_LDADD = $(COMMON_LIBS)

test_tcp_io_flush_SOURCES = test_tcp_io_flush.c
test_tcp_io_flush_LDADD = $(LIBUTUN) $(CRYPTO_LIBS) $(COMMON_LIBS)

test_chat_poll_SOURCES = test_chat_poll.c
test_chat_poll_CFLAGS = -I$(top_srcdir)/src -I$(top_srcdir)/src/chat -I$(top_srcdir)/lib
test_chat_poll_LDADD = $(LIBUTUN) $(CRYPTO_LIBS) $(COMMON_LIBS)
test_chat_poll_LDFLAGS = -Wl,--wrap=sqlite3_prepare_v2 -Wl,--wrap=sqlite3_bind_blob -Wl,--wrap=sqlite3_bind_text \
    -Wl,--wrap=sqlite3_step -Wl,--wrap=sqlite3_finalize -Wl,--wrap=u_malloc_impl

test_chat_poll_network_SOURCES = test_chat_poll_network.c
test_chat_poll_network_CFLAGS = -I$(top_srcdir)/src -I$(top_srcdir)/src/chat -I$(top_srcdir)/lib
test_chat_poll_network_LDADD = $(LIBUTUN) $(CRYPTO_LIBS) $(COMMON_LIBS)

test_reality_record_SOURCES = test_reality_record.c
test_reality_record_LDADD = $(LIBUTUN) $(COMMON_LIBS)

if OS_LINUX
# Низкий лимит проверяет реальную смену ключей в обе стороны без миллиона пакетов.
test_stcp_reality_rotation_SOURCES = test_stcp.c ../src/transport_layer/reality_io.c ../src/transport_layer/reality_record.c
test_stcp_reality_rotation_CFLAGS = $(AM_CFLAGS) -DREALITY_KEY_LIMIT=8
test_stcp_reality_rotation_LDFLAGS = -Wl,--wrap=send -Wl,--wrap=recv -Wl,--wrap=random_bytes -Wl,--wrap=u_malloc_impl -Wl,--wrap=u_calloc_impl
test_stcp_reality_rotation_LDADD = $(LIBUTUN) $(COMMON_LIBS)
endif

if OS_LINUX
test_reality_fallback_SOURCES = test_reality_fallback.c
test_reality_fallback_LDFLAGS = -Wl,--wrap=ntp_time_get_seconds
test_reality_fallback_LDADD = $(LIBUTUN) $(COMMON_LIBS)
endif

test_reality_tls_SOURCES = test_reality_tls.c
test_reality_tls_LDADD = $(LIBUTUN) -lssl $(COMMON_LIBS)

# TUN: изолированные отказы, проверка fd/памяти и модель Windows post на реальном uasync.
test_tun_if_SOURCES = test_tun_if.c $(async_regression_sources)
test_tun_if_LDADD = $(LIBUTUN) $(COMMON_LIBS)
test_tun_if_LDFLAGS = $(async_regression_ldflags) -Wl,--wrap=tun_platform_init -Wl,--wrap=tun_platform_write -Wl,--wrap=socketpair

if OS_LINUX
check_PROGRAMS += test_proxy_lifecycle
test_proxy_lifecycle_SOURCES = test_proxy_lifecycle.c $(async_regression_sources)
test_proxy_lifecycle_LDADD = $(LIBUTUN) $(COMMON_LIBS)
test_proxy_lifecycle_LDFLAGS = $(async_regression_ldflags) -Wl,--wrap=clock_gettime -Wl,--wrap=socket -Wl,--wrap=bind -Wl,--wrap=sendto -Wl,--wrap=connect -Wl,--wrap=setsockopt \
    -Wl,--wrap=socket_set_nonblocking -Wl,--wrap=uasync_add_socket_t -Wl,--wrap=uasync_set_timeout -Wl,--wrap=uasync_reserve_soon
endif

# Общий кодек без запуска сети; атомарность всех выходов и границы формата.
test_invite_codec_SOURCES = test_invite_codec.c
test_invite_codec_CFLAGS = -I$(top_srcdir)/src -I$(top_srcdir)/lib
test_invite_codec_LDADD = $(LIBUTUN) $(CRYPTO_LIBS) $(COMMON_LIBS)

test_object_link_SOURCES = test_object_link.c
test_object_link_CFLAGS = $(test_invite_codec_CFLAGS)
test_object_link_LDADD = $(test_invite_codec_LDADD)

check_PROGRAMS += test_server_join
test_server_join_SOURCES = test_server_join.c $(test_clock_sources)
test_server_join_LDADD = $(LIBUTUN) $(COMMON_LIBS)
test_server_join_LDFLAGS = $(test_clock_ldflags) -Wl,--wrap=etcp_send

# Настоящий controller/NCD/SQLite; транспорт и отдельные отказы перехвачены.
test_chat_join_faults_SOURCES = test_chat_join_faults.c test_join_wire.h
test_chat_join_faults_CFLAGS = -I$(top_srcdir)/src -I$(top_srcdir)/src/transport_layer -I$(top_srcdir)/src/routing_layer -I$(top_srcdir)/lib
test_chat_join_faults_LDADD = $(LIBUTUN) $(CRYPTO_LIBS) $(COMMON_LIBS)
test_chat_join_faults_LDFLAGS = -Wl,--wrap=node_conn_direct_open_node -Wl,--wrap=topo_groups_create_channel -Wl,--wrap=topo_group_add_node_cbk -Wl,--wrap=etcp_send -Wl,--wrap=uasync_set_timeout -Wl,--wrap=u_malloc_impl -Wl,--wrap=u_calloc_impl

test_etcp_driver_SOURCES = test_etcp_driver.c
test_etcp_driver_LDADD = $(LIBUTUN) $(COMMON_LIBS)

test_etcp_tx_rto_SOURCES = test_etcp_tx_rto.c $(test_clock_sources)
test_etcp_tx_rto_LDADD = $(LIBUTUN) $(COMMON_LIBS)
test_etcp_tx_rto_LDFLAGS = $(test_clock_ldflags)

check_PROGRAMS += test_devgroup
test_devgroup_SOURCES = test_devgroup.c
test_devgroup_LDADD = $(LIBUTUN) $(COMMON_LIBS)

check_PROGRAMS += test_devgroup_async
test_devgroup_async_SOURCES = test_devgroup_async.c
test_devgroup_async_CFLAGS = $(test_core_db_async_CFLAGS)
test_devgroup_async_LDADD = $(test_core_db_async_LDADD)
test_devgroup_async_LDFLAGS = -Wl,--wrap=etcp_route_send_key -Wl,--wrap=sqlite3_prepare_v2 -Wl,--wrap=sqlite3_exec

check_PROGRAMS += test_dm_identity
test_dm_identity_SOURCES = test_dm_identity.c
test_dm_identity_LDADD = $(LIBUTUN) $(COMMON_LIBS)

check_PROGRAMS += test_dm_schema
check_PROGRAMS += test_db_schema
test_db_schema_SOURCES = test_db_schema.c
test_db_schema_LDADD = $(LIBUTUN) $(COMMON_LIBS)
test_dm_schema_SOURCES = test_dm_schema.c
test_dm_schema_LDADD = $(LIBUTUN) $(COMMON_LIBS)

check_PROGRAMS += test_dm_groups
test_dm_groups_SOURCES = test_dm_groups.c
test_dm_groups_LDADD = $(LIBUTUN) $(COMMON_LIBS)

check_PROGRAMS += test_devgroup_network
test_devgroup_network_SOURCES = test_devgroup_network.c $(test_clock_sources)
test_devgroup_network_LDADD = $(LIBUTUN) $(COMMON_LIBS)
test_devgroup_network_LDFLAGS = $(test_clock_ldflags) -Wl,--wrap=etcp_route_send_key

check_PROGRAMS += test_chat_background_async
test_chat_background_async_SOURCES = test_chat_background_async.c
test_chat_background_async_LDADD = $(LIBUTUN) $(COMMON_LIBS)
test_chat_background_async_LDFLAGS = -Wl,--wrap=sqlite3_prepare_v2 -Wl,--wrap=sqlite3_exec -Wl,--wrap=sqlite3_get_table \
    -Wl,--wrap=sqlite3_finalize -Wl,--wrap=local_interfaces

check_PROGRAMS += test_admin_key_async
test_admin_key_async_SOURCES = test_admin_key_async.c
test_admin_key_async_CFLAGS = -I$(top_srcdir)/src -I$(top_srcdir)/lib
test_admin_key_async_LDADD = $(LIBUTUN) $(COMMON_LIBS)
if OS_LINUX
test_admin_key_async_LDFLAGS = -Wl,--wrap=sqlite3_prepare_v2 -Wl,--wrap=sqlite3_exec -Wl,--wrap=u_calloc_impl -Wl,--wrap=etcp_route_send_key
endif

check_PROGRAMS += test_dm_invite_async
test_dm_invite_async_SOURCES = test_dm_invite_async.c
test_dm_invite_async_CFLAGS = -I$(top_srcdir)/src -I$(top_srcdir)/lib
test_dm_invite_async_LDADD = $(LIBUTUN) $(COMMON_LIBS)
if OS_LINUX
test_dm_invite_async_LDFLAGS = -Wl,--wrap=sqlite3_prepare_v2 -Wl,--wrap=sqlite3_exec -Wl,--wrap=u_calloc_impl \
    -Wl,--wrap=chat_invite_build_link -Wl,--wrap=chat_invite_build_cancel
endif

check_PROGRAMS += test_service_worker_e2e
test_service_worker_e2e_SOURCES = test_service_worker_e2e.c
test_service_worker_e2e_CFLAGS = -I$(top_srcdir)/src -I$(top_srcdir)/lib
test_service_worker_e2e_LDADD = $(LIBUTUN) $(COMMON_LIBS)
if OS_LINUX
test_service_worker_e2e_LDFLAGS = -Wl,--wrap=sqlite3_prepare_v2 -Wl,--wrap=sqlite3_exec -Wl,--wrap=sqlite3_get_table -Wl,--wrap=db_async_run
endif

test_chat_log_SOURCES = test_chat_log.c
test_chat_log_CFLAGS = $(test_services_CFLAGS)
test_chat_log_LDADD = $(LIBUTUN) $(CRYPTO_LIBS) $(COMMON_LIBS)

# Пять настоящих узлов: аварийные INFO-гонки, дырки и тихие резервы.
test_stream_failover_SOURCES = $(test_stream_service_SOURCES)
test_stream_failover_CFLAGS = $(AM_CFLAGS) -DSTREAM_FAILOVER_TEST_ONLY
test_stream_failover_LDADD = $(test_stream_service_LDADD)
test_stream_failover_LDFLAGS = $(test_stream_service_LDFLAGS)

test_stream_overload_SOURCES = $(test_stream_service_SOURCES)
test_stream_overload_CFLAGS = $(AM_CFLAGS) -DSTREAM_OVERLOAD_TEST_ONLY
test_stream_overload_LDADD = $(test_stream_service_LDADD)
test_stream_overload_LDFLAGS = $(test_stream_service_LDFLAGS)
