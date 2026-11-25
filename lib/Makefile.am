noinst_LIBRARIES = libuasync.a

libuasync_a_SOURCES = \
	u_async.c \
	u_async.h \
	ll_queue.h \
	ll_queue.c \
	db_async.c \
	db_async.h \
	db_sql.c \
	db_sql.h \
	packet_link.c \
	packet_link.h \
	packet_link_backend.h \
	debug_config.c \
	debug_config.h \
	timeout_heap.c \
	timeout_heap.h \
	memory_pool.c \
	memory_pool.h \
	socket_compat.c \
	socket_compat.h \
	platform_compat.c \
	platform_compat.h \
	file_io.c \
	file_io.h \
	getmyip.c \
	getmyip.h \
	swm_min.c \
	swm_min.h \
	mem.c \
	mem.h \
	radix.c \
	radix.h \
	serialize.c \
	serialize.h \
	video_plane.c \
	video_plane.h \
	reed_solomon.c \
	reed_solomon.h \
	json_flat.c \
	json_flat.h \
	tcp_io.c \
	tcp_io.h \
	sqlite3.c \
	sqlite3.h \
	opus_codec.c \
	opus_codec.h \
	audio_compressor.c \
	audio_compressor.h \
	strbuf.c \
	strbuf.h \
	dns.c \
	dns.h \
	async_dns.c \
	async_dns.h \
	speex_aec.c \
	speex_aec.h \
	speex_ns.c \
	speex_ns.h \
	speexdsp/preprocess.c \
	speexdsp/filterbank.c \
	speexdsp/filterbank.h \
	speexdsp/include/speex/speex_preprocess.h \
	speexdsp/mdf.c \
	speexdsp/fftwrap.c \
	speexdsp/kiss_fft.c \
	speexdsp/kiss_fftr.c \
	silero_vad.c \
	silero_vad.h

libuasync_a_CFLAGS = \
	-D_ISOC99_SOURCE \
	-DDEBUG_OUTPUT_STDERR \
	-DSQLITE_THREADSAFE=1 \
	-DDNS_RANDOM=RAND_bytes \
	-DFLOATING_POINT \
	-DUSE_KISS_FFT \
	-DEXPORT= \
	-g \
	-I$(top_srcdir)/src \
	-I$(top_srcdir)/lib \
	-I$(top_srcdir)/lib/libopus/include \
	-I$(top_srcdir)/lib/speexdsp \
	-I$(top_srcdir)/lib/speexdsp/include

if HAVE_SILERO_VAD
libuasync_a_CFLAGS += -DHAVE_SILERO_VAD
endif

# Вшитая модель Silero VAD (включается из silero_vad.c)
EXTRA_DIST = silero_vad_model.inc \
	COPYING.silero-vad \
	wintun-LICENSE.txt \
	wintun-api-LICENSE.txt \
	speexdsp/COPYING \
	soundtouch-master/COPYING.TXT

# Clean build directory
clean-local:
	-rm -rf $(top_builddir)/build
