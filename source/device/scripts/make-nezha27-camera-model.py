#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Write the single measured main-camera model for the 24A446 source matcher.

The structure follows CMCapture's AVCaptureSession model and HakiOS's sensor
format matching. Values describe the stock OVX mode and the existing CPU
BGRA/NV12 scaler only; no Apple tuning or unavailable ISP modes are copied.
"""
import argparse
from pathlib import Path
import plistlib


def main_camera_model():
    stream = {'SensorWidth': 4096, 'SensorHeight': 3072,
              'MinFrameRate': 30, 'MaxFrameRate': 30, 'AutoFocusSystem': 0}
    outputs = {
        '12MP': {'Width': 4096, 'Height': 3072, 'CaptureStreamFormat': 'Photo'},
        '1080p30': {'Width': 1920, 'Height': 1080, 'CaptureStreamFormat': 'Photo'},
        '720p30': {'Width': 1280, 'Height': 720, 'CaptureStreamFormat': 'Photo'},
        '640x480': {'Width': 640, 'Height': 480, 'CaptureStreamFormat': 'Photo'},
    }
    for value in outputs.values():
        value.update(MaxFrameRateOverride=30, PixelFormatType='420f',
                     VideoStabilizationSupported=False, IrisSupported=False,
                     HDRSupported=False, SISSupported=False,
                     SushiRawSupported=False, DeepFusionSupported=False,
                     SemanticRenderingSupported=False, ZeroShutterLagSupported=False,
                     DeferredPhotoProcessingSupported=False)
    presets = {
        'Common': {'MaxFrameRate': 30, 'MinFrameRate': 30, 'VideoCodec': 'avc1',
                   'VideoCompressionProperties': {'MaxKeyFrameIntervalDuration': 1}},
        'AVCaptureSessionPresetPhoto': {'DeviceFormat': '12MP'},
        'AVCaptureSessionPreset1920x1080': {'DeviceFormat': '1080p30'},
        'AVCaptureSessionPreset1280x720': {'DeviceFormat': '720p30'},
        'AVCaptureSessionPreset640x480': {'DeviceFormat': '640x480'},
        'AVCaptureSessionPresetMedium': {'DeviceFormat': '640x480'},
        'AVCaptureSessionPresetLow': {'DeviceFormat': '640x480'},
        'AVCaptureSessionPresetHigh': 'AVCaptureSessionPreset1920x1080',
    }
    return {
        'AVCaptureDeviceCommonSettings': {
            'HEVCPreferred': False, 'faceTrackingSupported': False,
            'BackgroundReplacementSupported': False,
            'SemanticRenderingVersion': 0, 'livePhotoVitalityScoringSupported': False,
        },
        'AVCaptureDevices': [{
            'uniqueName': 'Back Camera', 'mediaType': 'vide',
            'hasAutoFocus': False, 'smoothAutoFocusSupported': False,
            'hasFlash': False, 'hasTorch': False, 'visionDataDeliverySupported': False,
            'MultiStream': {'AVCaptureDeviceFormats': outputs,
                            'FigCaptureStreamFormats': {'Photo': stream},
                            'AVCaptureSessionPresets': presets},
        }],
        'AVH264Settings': {}, 'AVHEVCSettings': {},
    }


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--out', type=Path, required=True)
    a = p.parse_args()
    a.out.parent.mkdir(parents=True, exist_ok=True)
    with a.out.open('xb') as f:
        plistlib.dump(main_camera_model(), f, fmt=plistlib.FMT_BINARY)
    print(a.out, 'one rear main sensor, 4096x3072 RAW10 at 30fps, CPU-scaled output presets')


if __name__ == '__main__':
    main()
