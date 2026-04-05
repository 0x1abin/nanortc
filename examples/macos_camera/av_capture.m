/*
 * av_capture — macOS AVFoundation camera + microphone capture
 *
 * SPDX-License-Identifier: MIT
 */

#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>

#include "av_capture.h"

#include <stdio.h>

/* ----------------------------------------------------------------
 * Delegate object: receives video and audio sample buffers
 * ---------------------------------------------------------------- */

@interface AVCaptureHandler
    : NSObject <AVCaptureVideoDataOutputSampleBufferDelegate, AVCaptureAudioDataOutputSampleBufferDelegate>
@property(nonatomic) av_capture_video_cb videoCb;
@property(nonatomic) av_capture_audio_cb audioCb;
@property(nonatomic) void *userdata;
@end

@implementation AVCaptureHandler

- (void)captureOutput:(AVCaptureOutput *)output
    didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer
           fromConnection:(AVCaptureConnection *)connection
{
    if ([output isKindOfClass:[AVCaptureVideoDataOutput class]]) {
        if (self.videoCb) {
            CVPixelBufferRef pixbuf = CMSampleBufferGetImageBuffer(sampleBuffer);
            CMTime pts = CMSampleBufferGetPresentationTimeStamp(sampleBuffer);
            self.videoCb(self.userdata, pixbuf, pts);
        }
    } else if ([output isKindOfClass:[AVCaptureAudioDataOutput class]]) {
        if (self.audioCb) {
            CMBlockBufferRef blockBuf = CMSampleBufferGetDataBuffer(sampleBuffer);
            if (!blockBuf)
                return;

            size_t totalLen = 0;
            char *dataPtr = NULL;
            OSStatus st = CMBlockBufferGetDataPointer(blockBuf, 0, NULL, &totalLen, &dataPtr);
            if (st != noErr || !dataPtr)
                return;

            CMFormatDescriptionRef fmt = CMSampleBufferGetFormatDescription(sampleBuffer);
            const AudioStreamBasicDescription *asbd = CMAudioFormatDescriptionGetStreamBasicDescription(fmt);
            if (!asbd)
                return;

            size_t sample_count = totalLen / (asbd->mBitsPerChannel / 8) / (size_t)asbd->mChannelsPerFrame;
            CMTime pts = CMSampleBufferGetPresentationTimeStamp(sampleBuffer);
            self.audioCb(self.userdata, (const int16_t *)dataPtr, sample_count, pts);
        }
    }
}

@end

/* ----------------------------------------------------------------
 * Module state
 * ---------------------------------------------------------------- */

static AVCaptureSession *s_session;
static AVCaptureHandler *s_handler;
static dispatch_queue_t s_video_queue;
static dispatch_queue_t s_audio_queue;

/* ----------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------- */

int av_capture_start(const av_capture_config_t *cfg)
{
    if (!cfg) {
        fprintf(stderr, "[av_capture] NULL config\n");
        return -1;
    }

    @autoreleasepool {
        s_session = [[AVCaptureSession alloc] init];

        /* Select capture preset based on requested resolution */
        if (cfg->video_width >= 1920 && cfg->video_height >= 1080)
            s_session.sessionPreset = AVCaptureSessionPreset1920x1080;
        else if (cfg->video_width >= 1280 && cfg->video_height >= 720)
            s_session.sessionPreset = AVCaptureSessionPreset1280x720;
        else
            s_session.sessionPreset = AVCaptureSessionPreset640x480;

        s_handler = [[AVCaptureHandler alloc] init];
        s_handler.videoCb = cfg->video_cb;
        s_handler.audioCb = cfg->audio_cb;
        s_handler.userdata = cfg->userdata;

        /* --- Video input --- */
        AVCaptureDevice *videoDev =
            [AVCaptureDevice defaultDeviceWithMediaType:AVMediaTypeVideo];
        if (!videoDev) {
            fprintf(stderr, "[av_capture] No camera found\n");
            return -1;
        }

        NSError *err = nil;
        AVCaptureDeviceInput *videoInput =
            [AVCaptureDeviceInput deviceInputWithDevice:videoDev error:&err];
        if (!videoInput) {
            fprintf(stderr, "[av_capture] Camera input error: %s\n",
                    err.localizedDescription.UTF8String);
            return -1;
        }
        if ([s_session canAddInput:videoInput]) {
            [s_session addInput:videoInput];
        } else {
            fprintf(stderr, "[av_capture] Cannot add camera input\n");
            return -1;
        }

        /* Configure frame rate */
        [videoDev lockForConfiguration:&err];
        if (!err) {
            videoDev.activeVideoMinFrameDuration =
                CMTimeMake(1, cfg->video_fps > 0 ? cfg->video_fps : 30);
            videoDev.activeVideoMaxFrameDuration =
                CMTimeMake(1, cfg->video_fps > 0 ? cfg->video_fps : 30);
            [videoDev unlockForConfiguration];
        }

        /* Video output (NV12 for zero-copy to VideoToolbox) */
        AVCaptureVideoDataOutput *videoOutput = [[AVCaptureVideoDataOutput alloc] init];
        videoOutput.videoSettings = @{
            (NSString *)kCVPixelBufferPixelFormatTypeKey :
                @(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange)
        };
        videoOutput.alwaysDiscardsLateVideoFrames = YES;

        s_video_queue = dispatch_queue_create("nanortc.video_capture", DISPATCH_QUEUE_SERIAL);
        [videoOutput setSampleBufferDelegate:s_handler queue:s_video_queue];

        if ([s_session canAddOutput:videoOutput]) {
            [s_session addOutput:videoOutput];
        } else {
            fprintf(stderr, "[av_capture] Cannot add video output\n");
            return -1;
        }

        /* --- Audio input --- */
        AVCaptureDevice *audioDev =
            [AVCaptureDevice defaultDeviceWithMediaType:AVMediaTypeAudio];
        if (!audioDev) {
            fprintf(stderr, "[av_capture] No microphone found\n");
            return -1;
        }

        AVCaptureDeviceInput *audioInput =
            [AVCaptureDeviceInput deviceInputWithDevice:audioDev error:&err];
        if (!audioInput) {
            fprintf(stderr, "[av_capture] Microphone input error: %s\n",
                    err.localizedDescription.UTF8String);
            return -1;
        }
        if ([s_session canAddInput:audioInput]) {
            [s_session addInput:audioInput];
        } else {
            fprintf(stderr, "[av_capture] Cannot add microphone input\n");
            return -1;
        }

        /* Audio output (16-bit PCM) */
        AVCaptureAudioDataOutput *audioOutput = [[AVCaptureAudioDataOutput alloc] init];
        /* Configure PCM format: 48kHz, mono, 16-bit signed integer */
        NSDictionary *audioSettings = @{
            AVFormatIDKey : @(kAudioFormatLinearPCM),
            AVSampleRateKey : @(cfg->audio_sample_rate > 0 ? cfg->audio_sample_rate : 48000),
            AVNumberOfChannelsKey : @(cfg->audio_channels > 0 ? cfg->audio_channels : 1),
            AVLinearPCMBitDepthKey : @16,
            AVLinearPCMIsNonInterleaved : @NO,
            AVLinearPCMIsFloatKey : @NO,
            AVLinearPCMIsBigEndianKey : @NO,
        };
        audioOutput.audioSettings = audioSettings;

        s_audio_queue = dispatch_queue_create("nanortc.audio_capture", DISPATCH_QUEUE_SERIAL);
        [audioOutput setSampleBufferDelegate:s_handler queue:s_audio_queue];

        if ([s_session canAddOutput:audioOutput]) {
            [s_session addOutput:audioOutput];
        } else {
            fprintf(stderr, "[av_capture] Cannot add audio output\n");
            return -1;
        }

        /* --- Start --- */
        [s_session startRunning];
        fprintf(stderr, "[av_capture] Capture started (video=%dx%d@%dfps, audio=%dHz/%dch)\n",
                cfg->video_width, cfg->video_height,
                cfg->video_fps > 0 ? cfg->video_fps : 30,
                cfg->audio_sample_rate > 0 ? cfg->audio_sample_rate : 48000,
                cfg->audio_channels > 0 ? cfg->audio_channels : 1);
    }

    return 0;
}

void av_capture_stop(void)
{
    @autoreleasepool {
        if (s_session) {
            [s_session stopRunning];
            s_session = nil;
            fprintf(stderr, "[av_capture] Capture stopped\n");
        }
        s_handler = nil;
        s_video_queue = nil;
        s_audio_queue = nil;
    }
}
