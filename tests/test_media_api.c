/* Public media mutation boundaries. SPDX-License-Identifier: MIT */
#include "nanortc.h"
#include "nano_test.h"
#include <string.h>

#if NANORTC_HAVE_MEDIA_TRANSPORT
static nanortc_t rtc;

TEST(test_media_mutations_validate_and_keep_sdp_direction_consistent)
{
    memset(&rtc, 0, sizeof(rtc));
#if NANORTC_FEATURE_VIDEO
    int mid = nanortc_add_video_track(&rtc, NANORTC_DIR_SENDONLY, NANORTC_CODEC_H264);
#else
    int mid = nanortc_add_audio_track(&rtc, NANORTC_DIR_SENDONLY, NANORTC_CODEC_OPUS, 48000, 2);
#endif
    ASSERT_TRUE(mid >= 0);
    ASSERT_OK(nanortc_set_direction(&rtc, (uint8_t)mid, NANORTC_DIR_RECVONLY));
    ASSERT_EQ(rtc.media[0].direction, NANORTC_DIR_RECVONLY);
    ASSERT_EQ(sdp_find_mline(&rtc.sdp, (uint8_t)mid)->direction, NANORTC_DIR_RECVONLY);
    ASSERT_EQ(nanortc_set_direction(&rtc, (uint8_t)mid, (nanortc_direction_t)99),
              NANORTC_ERR_INVALID_PARAM);
    ASSERT_EQ(nanortc_set_direction(&rtc, 255, NANORTC_DIR_SENDONLY), NANORTC_ERR_INVALID_PARAM);
    rtc.signaling_state = NANORTC_SIGNALING_HAVE_LOCAL_OFFER;
    ASSERT_EQ(nanortc_set_direction(&rtc, (uint8_t)mid, NANORTC_DIR_SENDONLY),
              NANORTC_ERR_NOT_IMPLEMENTED);
#if NANORTC_FEATURE_VIDEO
    ASSERT_EQ(nanortc_add_video_track(&rtc, NANORTC_DIR_SENDONLY, NANORTC_CODEC_H264),
              NANORTC_ERR_NOT_IMPLEMENTED);
#else
    ASSERT_EQ(nanortc_add_audio_track(&rtc, NANORTC_DIR_SENDONLY, NANORTC_CODEC_OPUS, 48000, 2),
              NANORTC_ERR_NOT_IMPLEMENTED);
#endif
    ASSERT_EQ(rtc.media_count, 1);
    ASSERT_EQ(rtc.media[0].direction, NANORTC_DIR_RECVONLY);
    rtc.signaling_state = NANORTC_SIGNALING_READY;
    rtc.state = NANORTC_STATE_DTLS_CONNECTED;
    ASSERT_EQ(nanortc_set_direction(&rtc, (uint8_t)mid, NANORTC_DIR_SENDONLY),
              NANORTC_ERR_NOT_IMPLEMENTED);
}

TEST(test_media_add_rejects_wrong_codec_kind_without_mutation)
{
    memset(&rtc, 0, sizeof(rtc));
    ASSERT_EQ(nanortc_add_video_track(&rtc, NANORTC_DIR_SENDONLY, NANORTC_CODEC_OPUS),
              NANORTC_ERR_INVALID_PARAM);
    ASSERT_EQ(nanortc_add_audio_track(&rtc, NANORTC_DIR_SENDONLY, NANORTC_CODEC_H264, 48000, 2),
              NANORTC_ERR_INVALID_PARAM);
    ASSERT_EQ(nanortc_add_audio_track(&rtc, NANORTC_DIR_SENDONLY, NANORTC_CODEC_OPUS, 0, 2),
              NANORTC_ERR_INVALID_PARAM);
    ASSERT_EQ(nanortc_add_audio_track(&rtc, NANORTC_DIR_SENDONLY, NANORTC_CODEC_OPUS, 48000, 0),
              NANORTC_ERR_INVALID_PARAM);
    ASSERT_EQ(rtc.media_count, 0);
    ASSERT_EQ(rtc.sdp.mline_count, 0);
}
#endif

TEST_MAIN_BEGIN("nanortc media API boundaries")
#if NANORTC_HAVE_MEDIA_TRANSPORT
RUN(test_media_mutations_validate_and_keep_sdp_direction_consistent);
RUN(test_media_add_rejects_wrong_codec_kind_without_mutation);
#endif
TEST_MAIN_END
