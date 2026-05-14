#import <borealis/core/logger.hpp>
#import <borealis/platforms/desktop/desktop_platform.hpp>
#import <UIKit/UIKit.h>

#import <CoreHaptics/CoreHaptics.h>
#import <GameController/GameController.h>

@interface HapticContext : NSObject

-(void)setMotorAmplitude:(unsigned short)amplitude;
-(void)cleanup;

+(HapticContext*) createContextForHighFreqMotor;
+(HapticContext*) createContextForLowFreqMotor;
+(HapticContext*) createContextForLeftTrigger;
+(HapticContext*) createContextForRightTrigger;

@end

@implementation HapticContext {
    GCControllerPlayerIndex _playerIndex;
    CHHapticEngine* _hapticEngine API_AVAILABLE(ios(13.0), tvos(14.0));
    id<CHHapticPatternPlayer> _hapticPlayer API_AVAILABLE(ios(13.0), tvos(14.0));
    BOOL _playing;
}

-(void)cleanup API_AVAILABLE(ios(14.0), tvos(14.0)) {
    if (_hapticPlayer != nil) {
        [_hapticPlayer cancelAndReturnError:nil];
        _hapticPlayer = nil;
    }
    if (_hapticEngine != nil) {
        [_hapticEngine stopWithCompletionHandler:nil];
        _hapticEngine = nil;
    }
}

-(void)setMotorAmplitude:(unsigned short)amplitude API_AVAILABLE(ios(14.0), tvos(14.0)) {
    NSError* error;

    // Check if the haptic engine died
    if (_hapticEngine == nil) {
        return;
    }

    // Stop the effect entirely if the amplitude is 0
    if (amplitude == 0) {
        if (_playing) {
            [_hapticPlayer stopAtTime:0 error:&error];
            _playing = NO;
        }

        return;
    }

    if (_hapticPlayer == nil) {
        // We must initialize the intensity to 1.0f because the dynamic parameters are multiplied by this value before being applied
        CHHapticEventParameter* intensityParameter = [[CHHapticEventParameter alloc] initWithParameterID:CHHapticEventParameterIDHapticIntensity value:1.0f];
        CHHapticEvent* hapticEvent = [[CHHapticEvent alloc] initWithEventType:CHHapticEventTypeHapticContinuous parameters:[NSArray arrayWithObject:intensityParameter] relativeTime:0 duration:GCHapticDurationInfinite];
        CHHapticPattern* hapticPattern = [[CHHapticPattern alloc] initWithEvents:[NSArray arrayWithObject:hapticEvent] parameters:[[NSArray alloc] init] error:&error];
        if (error != nil) {
            NSLog(@"Controller %ld: Haptic pattern creation failed: %@", static_cast<long>(_playerIndex), error);
            return;
        }

        _hapticPlayer = [_hapticEngine createPlayerWithPattern:hapticPattern error:&error];
        if (error != nil) {
            NSLog(@"Controller %ld: Haptic player creation failed: %@", static_cast<long>(_playerIndex), error);
            return;
        }
    }

    CHHapticDynamicParameter* intensityParameter = [[CHHapticDynamicParameter alloc] initWithParameterID:CHHapticDynamicParameterIDHapticIntensityControl value:amplitude / 65535.0f relativeTime:0];
    [_hapticPlayer sendParameters:[NSArray arrayWithObject:intensityParameter] atTime:CHHapticTimeImmediate error:&error];
    if (error != nil) {
        NSLog(@"Controller %ld: Haptic player parameter update failed: %@", static_cast<long>(_playerIndex), error);
        return;
    }

    if (!_playing) {
        [_hapticPlayer startAtTime:0 error:&error];
        if (error != nil) {
            _hapticPlayer = nil;
            NSLog(@"Controller %ld: Haptic playback start failed: %@", static_cast<long>(_playerIndex), error);
            return;
        }

        _playing = YES;
    }
}

-(id) initWithLocality:(GCHapticsLocality)locality API_AVAILABLE(ios(14.0), tvos(14.0)) {
    NSLog(@"Controller %d does not support haptic locality: %@", 0, locality);
    if (@available(iOS 13.0, *)) {
        NSError *error = nil;
        _hapticEngine = [[CHHapticEngine alloc] initAndReturnError:&error];
    } else{
        return nil;
    }
    _playerIndex = GCControllerPlayerIndex1;

    NSError* error;
    [_hapticEngine startAndReturnError:&error];
    if (error != nil) {
        NSLog(@"Controller %ld: Haptic engine failed to start: %@", static_cast<long>(_playerIndex), error);
        return nil;
    }

    __weak typeof(self) weakSelf = self;
    _hapticEngine.stoppedHandler = ^(CHHapticEngineStoppedReason stoppedReason) {
        HapticContext* me = weakSelf;
        if (me == nil) {
            return;
        }

        NSLog(@"Controller %ld: Haptic engine stopped: %ld", static_cast<long>(me->_playerIndex), static_cast<long>(stoppedReason));
        me->_hapticPlayer = nil;
        me->_hapticEngine = nil;
        me->_playing = NO;
    };
    _hapticEngine.resetHandler = ^{
        HapticContext* me = weakSelf;
        if (me == nil) {
            return;
        }

        NSLog(@"Controller %ld: Haptic engine reset", static_cast<long>(me->_playerIndex));
        me->_hapticPlayer = nil;
        me->_playing = NO;
        [me->_hapticEngine startAndReturnError:nil];
    };

    return self;
}

+(HapticContext*) createContextForHighFreqMotor {
    if (@available(iOS 14.0, tvOS 14.0, *)) {
        return [[HapticContext alloc] initWithLocality:GCHapticsLocalityRightHandle];
    }
    else {
        return nil;
    }
}

+(HapticContext*) createContextForLowFreqMotor {
    if (@available(iOS 14.0, tvOS 14.0, *)) {
        return [[HapticContext alloc] initWithLocality:GCHapticsLocalityLeftHandle];
    }
    else {
        return nil;
    }
}

+(HapticContext*) createContextForLeftTrigger {
    if (@available(iOS 14.0, tvOS 14.0, *)) {
        return [[HapticContext alloc] initWithLocality:GCHapticsLocalityLeftTrigger];
    }
    else {
        return nil;
    }
}

+(HapticContext*) createContextForRightTrigger {
    if (@available(iOS 14.0, tvOS 14.0, *)) {
        return [[HapticContext alloc] initWithLocality:GCHapticsLocalityRightTrigger];
    }
    else {
        return nil;
    }
}

@end

namespace brls {

ThemeVariant ios_theme() {
    UIUserInterfaceStyle userInterfaceStyle = UIUserInterfaceStyleUnspecified;

#if PLATFORM_VISIONOS
    userInterfaceStyle = UITraitCollection.currentTraitCollection.userInterfaceStyle;
#else
    userInterfaceStyle = UIScreen.mainScreen.traitCollection.userInterfaceStyle;
#endif

    if (userInterfaceStyle == UIUserInterfaceStyleDark)
        return ThemeVariant::DARK;
    else
        return ThemeVariant::LIGHT;
}

bool darwin_runloop(const std::function<bool()>& runLoopImpl) {
    @autoreleasepool {
        return runLoopImpl();
    }
}

uint8_t ios_battery_status() {
#if defined(IOS)
    UIDevice.currentDevice.batteryMonitoringEnabled = true;
    return UIDevice.currentDevice.batteryState;
#else
    return 0;
#endif
}

void ios_openURL(std::string url) {
#if PLATFORM_IOS || PLATFORM_TVOS || PLATFORM_VISIONOS
    [UIApplication.sharedApplication openURL:[NSURL URLWithString: [NSString stringWithCString:url.c_str() encoding:NSUTF8StringEncoding]] options:@{} completionHandler:^(BOOL success){}];
#endif
}

float ios_battery() {
#if defined(IOS)
    return UIDevice.currentDevice.batteryLevel;
#else
    return 0;
#endif
}

HapticContext* contextForHighFreqMotor = NULL;
HapticContext* contextForLowFreqMotor = NULL;

void init_device_rumble() {
    if (contextForHighFreqMotor == NULL)
        contextForHighFreqMotor = [HapticContext createContextForLowFreqMotor];

    if (contextForLowFreqMotor == NULL)
        contextForLowFreqMotor = [HapticContext createContextForHighFreqMotor];
}

void device_rumble(unsigned short lowFreqMotor, unsigned short highFreqMotor) {
    [contextForLowFreqMotor setMotorAmplitude:lowFreqMotor];
    [contextForHighFreqMotor setMotorAmplitude:highFreqMotor];
}

};
