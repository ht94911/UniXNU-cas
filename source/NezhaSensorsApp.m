// SPDX-License-Identifier: BSD-2-Clause-Patent
#import <UIKit/UIKit.h>
#import <CoreMotion/CoreMotion.h>
#include <unistd.h>

@interface SensorsController : UIViewController
@property(nonatomic,strong) CMMotionManager *motion;
@property(nonatomic,strong) NSOperationQueue *queue;
@property(nonatomic,strong) UITextView *output;
@property(nonatomic,strong) UILabel *directions;
@property(nonatomic,strong) NSFileHandle *log;
@property(nonatomic,strong) NSTimer *timer;
@property(nonatomic,strong) NSMutableDictionary *samples;
@property(nonatomic) BOOL rawEnabled,motionEnabled;
@property(nonatomic) NSUInteger marker;
@property(nonatomic) NSUInteger refreshCount;
@property(nonatomic) NSUInteger lifecycleStage,lifecycleTicks;
@property(nonatomic) BOOL lifecycleStarted,lifecycleDone,lifecyclePassed;
@property(nonatomic,strong) NSDictionary *lifecycleCheckpoint;
@property(nonatomic,copy) NSString *interfaceCheckStatus;
@property(nonatomic) UIInterfaceOrientation requestedInterface;
@property(nonatomic) NSTimeInterval interfaceCheckStarted;
@property(nonatomic) BOOL interfaceCheckRunning;
@end

@implementation SensorsController
- (UIInterfaceOrientationMask)supportedInterfaceOrientations
{
    return UIInterfaceOrientationMaskPortrait|UIInterfaceOrientationMaskLandscapeLeft|UIInterfaceOrientationMaskLandscapeRight;
}
- (BOOL)shouldAutorotate { return YES; }
- (void)restoreAutomaticInterface
{
    UIWindowScene *scene=self.view.window.windowScene;
    self.interfaceCheckRunning=NO;
    self.requestedInterface=UIInterfaceOrientationUnknown;
    if(!scene)return;
    [self setNeedsUpdateOfSupportedInterfaceOrientations];
    UIWindowSceneGeometryPreferencesIOS *preferences=[[UIWindowSceneGeometryPreferencesIOS alloc]
        initWithInterfaceOrientations:self.supportedInterfaceOrientations];
    __weak SensorsController *weak=self;
    [scene requestGeometryUpdateWithPreferences:preferences errorHandler:^(NSError *error){
        [weak record:@{@"interfaceCheck":@"automatic-range-error",@"error":@(error.code),
            @"domain":error.domain?:@"",@"uptime":@(NSProcessInfo.processInfo.systemUptime)}];
    }];
    [self record:@{@"interfaceCheck":@"automatic-range",@"supportedMask":@(self.supportedInterfaceOrientations),
        @"uptime":@(NSProcessInfo.processInfo.systemUptime)}];
}
- (void)checkInterfaceRotation
{
    if(!self.lifecycleDone||self.interfaceCheckRunning)return;
    self.interfaceCheckRunning=YES;
    [self record:@{@"interfaceCheck":@"start",@"uptime":@(NSProcessInfo.processInfo.systemUptime),
        @"deviceOrientation":@(UIDevice.currentDevice.orientation),
        @"supportedMask":@(self.supportedInterfaceOrientations)}];
    [self requestInterface:UIInterfaceOrientationLandscapeLeft];
}
- (void)requestInterface:(UIInterfaceOrientation)orientation
{
    UIWindowScene *scene=self.view.window.windowScene;
    if(!scene){self.interfaceCheckRunning=NO;self.interfaceCheckStatus=@"未完成：窗口不可用";return;}
    self.requestedInterface=orientation;self.interfaceCheckStarted=NSProcessInfo.processInfo.systemUptime;
    self.interfaceCheckStatus=orientation==UIInterfaceOrientationPortrait?@"正在返回竖屏":@"正在检查横屏";
    [self setNeedsUpdateOfSupportedInterfaceOrientations];
    UIInterfaceOrientationMask mask=(UIInterfaceOrientationMask)(1UL<<orientation);
    UIWindowSceneGeometryPreferencesIOS *preferences=[[UIWindowSceneGeometryPreferencesIOS alloc] initWithInterfaceOrientations:mask];
    __weak SensorsController *weak=self;
    [scene requestGeometryUpdateWithPreferences:preferences errorHandler:^(NSError *error){
        SensorsController *controller=weak;if(!controller)return;
        [controller record:@{@"interfaceCheck":@"error",@"target":@(orientation),
            @"error":@(error.code),@"domain":error.domain?:@"",@"description":error.localizedDescription?:@"",
            @"uptime":@(NSProcessInfo.processInfo.systemUptime)}];
        controller.requestedInterface=UIInterfaceOrientationUnknown;
        controller.interfaceCheckStatus=@"未通过：界面请求返回错误";
        [controller restoreAutomaticInterface];
    }];
}
- (void)stepInterfaceCheck
{
    if(self.requestedInterface==UIInterfaceOrientationUnknown)return;
    UIInterfaceOrientation actual=self.view.window.windowScene.effectiveGeometry.interfaceOrientation;
    NSTimeInterval elapsed=NSProcessInfo.processInfo.systemUptime-self.interfaceCheckStarted;
    BOOL layoutMatches=actual==UIInterfaceOrientationPortrait?
        self.view.bounds.size.height>self.view.bounds.size.width:
        self.view.bounds.size.width>self.view.bounds.size.height;
    if(actual==self.requestedInterface&&layoutMatches){
        UIInterfaceOrientation target=self.requestedInterface;
        [self record:@{@"interfaceCheck":@"observed",@"target":@(target),@"actual":@(actual),
            @"width":@(self.view.bounds.size.width),@"height":@(self.view.bounds.size.height),
            @"deviceOrientation":@(UIDevice.currentDevice.orientation),@"uptime":@(NSProcessInfo.processInfo.systemUptime)}];
        self.requestedInterface=UIInterfaceOrientationUnknown;
        if(target!=UIInterfaceOrientationPortrait){
            self.interfaceCheckStatus=@"横屏已显示，正在返回竖屏";
            __weak SensorsController *weak=self;
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW,(int64_t)(3*NSEC_PER_SEC)),dispatch_get_main_queue(),^{
                [weak requestInterface:UIInterfaceOrientationPortrait];
            });
        }else {
            self.interfaceCheckStatus=@"通过：横屏与竖屏均已显示";
            [self restoreAutomaticInterface];
        }
    }else if(elapsed>=8){
        [self record:@{@"interfaceCheck":@"timeout",@"target":@(self.requestedInterface),@"actual":@(actual),
            @"uptime":@(NSProcessInfo.processInfo.systemUptime)}];
        self.requestedInterface=UIInterfaceOrientationUnknown;self.interfaceCheckStatus=@"未通过：方向未改变";
        [self restoreAutomaticInterface];
    }
}
- (void)record:(NSDictionary *)value
{
    NSData *data=[NSJSONSerialization dataWithJSONObject:value options:NSJSONWritingSortedKeys error:NULL];
    if(!data)return;
    @try { [self.log writeData:data]; [self.log writeData:[@"\n" dataUsingEncoding:NSUTF8StringEncoding]]; }
    @catch(NSException *e) { NSLog(@"Sensors log failed %@",e.name); }
}
- (void)sample:(NSString *)kind time:(NSTimeInterval)t values:(NSArray *)values error:(NSError *)error
{
    @synchronized(self.samples) {
        NSDictionary *last=self.samples[kind];
        double previous=[last[@"t"] doubleValue];
        self.samples[kind]=@{@"kind":kind,@"t":@(t),@"v":values ?: @[],
            @"dt":@(previous>0&&t>0?t-previous:0),@"n":@([last[@"n"] unsignedLongLongValue]+(t>0&&values.count?1:0)),
            @"valid":@(t>0&&values.count&&error==nil),@"error":@(error.code)};
    }
}
- (void)setRawEnabled:(BOOL)enabled
{
    if(_rawEnabled==enabled)return;
    _rawEnabled=enabled;
    if(enabled) {
        __weak SensorsController *weak=self;
        [self.motion startAccelerometerUpdatesToQueue:self.queue withHandler:^(CMAccelerometerData *data,NSError *error) {
            if(!data){[weak sample:@"accel" time:0 values:nil error:error];return;}
            [weak sample:@"accel" time:data.timestamp values:@[@(data.acceleration.x),@(data.acceleration.y),@(data.acceleration.z)] error:error];
        }];
        [self.motion startGyroUpdatesToQueue:self.queue withHandler:^(CMGyroData *data,NSError *error) {
            if(!data){[weak sample:@"gyro" time:0 values:nil error:error];return;}
            [weak sample:@"gyro" time:data.timestamp values:@[@(data.rotationRate.x),@(data.rotationRate.y),@(data.rotationRate.z)] error:error];
        }];
    } else { [self.motion stopAccelerometerUpdates]; [self.motion stopGyroUpdates]; }
    [self record:@{@"control":@"raw",@"enabled":@(enabled)}];
}
- (void)setMotionEnabled:(BOOL)enabled
{
    if(_motionEnabled==enabled)return;
    _motionEnabled=enabled;
    if(enabled) {
        __weak SensorsController *weak=self;
        [self.motion startDeviceMotionUpdatesUsingReferenceFrame:CMAttitudeReferenceFrameXArbitraryZVertical
            toQueue:self.queue withHandler:^(CMDeviceMotion *data,NSError *error) {
                if(!data){[weak sample:@"motion" time:0 values:nil error:error];return;}
                CMQuaternion q=data.attitude.quaternion;
                [weak sample:@"motion" time:data.timestamp values:@[@(data.gravity.x),@(data.gravity.y),@(data.gravity.z),
                    @(data.userAcceleration.x),@(data.userAcceleration.y),@(data.userAcceleration.z),
                    @(data.rotationRate.x),@(data.rotationRate.y),@(data.rotationRate.z),@(q.x),@(q.y),@(q.z),@(q.w)] error:error];
            }];
    } else [self.motion stopDeviceMotionUpdates];
    [self record:@{@"control":@"motion",@"enabled":@(enabled)}];
}
- (void)cancelLifecycleCheck:(NSString *)reason
{
    if(!self.lifecycleStarted||self.lifecycleDone)return;
    self.lifecycleDone=YES;self.lifecyclePassed=NO;
    [self record:@{@"lifecycleCheck":@"interrupted",@"reason":reason,@"stage":@(self.lifecycleStage),
        @"uptime":@(NSProcessInfo.processInfo.systemUptime)}];
}
- (void)toggleRaw { [self cancelLifecycleCheck:@"manual-control"];self.rawEnabled=!self.rawEnabled; }
- (void)toggleMotion { [self cancelLifecycleCheck:@"manual-control"];self.motionEnabled=!self.motionEnabled; }
- (void)applyLifecycleStage
{
    static const BOOL raw[]={YES,NO,YES,YES,YES,NO,YES};
    static const BOOL fusion[]={YES,YES,YES,NO,YES,NO,YES};
    self.lifecycleTicks=0;self.lifecycleCheckpoint=nil;
    self.rawEnabled=raw[self.lifecycleStage];self.motionEnabled=fusion[self.lifecycleStage];
    [self record:@{@"lifecycleCheck":@"begin",@"stage":@(self.lifecycleStage),
        @"raw":@(self.rawEnabled),@"motion":@(self.motionEnabled),@"uptime":@(NSProcessInfo.processInfo.systemUptime)}];
}
- (void)startLifecycleCheck
{
    if(self.lifecycleStarted)return;
    self.lifecycleStarted=YES;self.lifecyclePassed=YES;self.lifecycleStage=0;
    [self applyLifecycleStage];
}
- (void)stepLifecycleCheck
{
    if(!self.lifecycleStarted||self.lifecycleDone||UIApplication.sharedApplication.applicationState!=UIApplicationStateActive)return;
    self.lifecycleTicks++;
    if(self.lifecycleTicks==3) {
        @synchronized(self.samples) { self.lifecycleCheckpoint=[self.samples copy]; }
    }
    if(self.lifecycleTicks<10)return;
    NSMutableDictionary *streams=[NSMutableDictionary new];BOOL passed=YES;
    @synchronized(self.samples) {
        for(NSString *kind in @[@"accel",@"gyro",@"motion"]) {
            NSDictionary *before=self.lifecycleCheckpoint[kind],*after=self.samples[kind];
            unsigned long long first=[before[@"n"] unsignedLongLongValue],last=[after[@"n"] unsignedLongLongValue];
            BOOL enabled=[kind isEqualToString:@"motion"]?self.motionEnabled:self.rawEnabled;
            BOOL advancing=last>first&&[after[@"valid"] boolValue]&&[after[@"t"] doubleValue]>[before[@"t"] doubleValue];
            BOOL streamPassed=enabled?advancing:last==first;
            streams[kind]=@{@"enabled":@(enabled),@"before":@(first),@"after":@(last),@"passed":@(streamPassed)};
            passed&=streamPassed;
        }
    }
    [self record:@{@"lifecycleCheck":@"result",@"stage":@(self.lifecycleStage),@"streams":streams,
        @"passed":@(passed),@"uptime":@(NSProcessInfo.processInfo.systemUptime)}];
    self.lifecyclePassed&=passed;
    if(self.lifecycleStage==6) {
        self.lifecycleDone=YES;
        [self record:@{@"lifecycleCheck":@"complete",@"passed":@(self.lifecyclePassed),
            @"uptime":@(NSProcessInfo.processInfo.systemUptime)}];
    } else { self.lifecycleStage++;[self applyLifecycleStage]; }
}
- (void)mark { self.marker++; [self record:@{@"marker":@(self.marker),@"uptime":@(NSProcessInfo.processInfo.systemUptime)}]; }
- (void)proximity:(NSNotification *)notification
{
    (void)notification;
    [self record:@{@"kind":@"proximity",@"near":@(UIDevice.currentDevice.proximityState),@"uptime":@(NSProcessInfo.processInfo.systemUptime)}];
}
- (void)directionChanged:(NSNotification *)notification
{
    (void)notification;
    [self record:@{@"orientationEvent":@"device",@"deviceOrientation":@(UIDevice.currentDevice.orientation),
        @"interfaceOrientation":@(self.view.window.windowScene.effectiveGeometry.interfaceOrientation),
        @"uptime":@(NSProcessInfo.processInfo.systemUptime)}];
    [self updateDisplay];
}
- (void)viewWillTransitionToSize:(CGSize)size withTransitionCoordinator:(id<UIViewControllerTransitionCoordinator>)coordinator
{
    [super viewWillTransitionToSize:size withTransitionCoordinator:coordinator];
    [self record:@{@"orientationEvent":@"transition-begin",@"width":@(size.width),@"height":@(size.height),
        @"deviceOrientation":@(UIDevice.currentDevice.orientation),@"uptime":@(NSProcessInfo.processInfo.systemUptime)}];
    __weak SensorsController *weak=self;
    [coordinator animateAlongsideTransition:nil completion:^(id<UIViewControllerTransitionCoordinatorContext> context){
        SensorsController *controller=weak;if(!controller)return;
        [controller record:@{@"orientationEvent":@"transition-end",@"cancelled":@(context.isCancelled),
            @"deviceOrientation":@(UIDevice.currentDevice.orientation),
            @"interfaceOrientation":@(controller.view.window.windowScene.effectiveGeometry.interfaceOrientation),
            @"width":@(controller.view.bounds.size.width),@"height":@(controller.view.bounds.size.height),
            @"uptime":@(NSProcessInfo.processInfo.systemUptime)}];
        [controller updateDisplay];
    }];
}
- (void)refresh
{
    self.refreshCount++;
    [self stepLifecycleCheck];
    [self stepInterfaceCheck];
    [self updateDisplay];
}
- (void)updateDisplay
{
    const UIDeviceOrientation device=UIDevice.currentDevice.orientation;
    const UIInterfaceOrientation interface=self.view.window.windowScene.effectiveGeometry.interfaceOrientation;
    self.directions.text=[NSString stringWithFormat:@"界面方向：%ld　设备方向：%ld",(long)interface,(long)device];
    [self record:@{@"status":@1,@"refresh":@(self.refreshCount),@"uptime":@(NSProcessInfo.processInfo.systemUptime),
        @"applicationState":@(UIApplication.sharedApplication.applicationState),@"accelAvailable":@(self.motion.accelerometerAvailable),
        @"gyroAvailable":@(self.motion.gyroAvailable),@"motionAvailable":@(self.motion.deviceMotionAvailable),
        @"accelActive":@(self.motion.accelerometerActive),@"gyroActive":@(self.motion.gyroActive),
        @"motionActive":@(self.motion.deviceMotionActive),@"proximityEnabled":@(UIDevice.currentDevice.proximityMonitoringEnabled),
        @"deviceOrientation":@(device),@"interfaceOrientation":@(interface),
        @"displayedDirections":self.directions.text?:@"",
        @"interfaceCheckRunning":@(self.interfaceCheckRunning),
        @"supportedInterfaceMask":@(self.supportedInterfaceOrientations),
        @"viewWidth":@(self.view.bounds.size.width),@"viewHeight":@(self.view.bounds.size.height),
        @"deviceOrientationNotificationsEnabled":@(UIDevice.currentDevice.generatesDeviceOrientationNotifications),
        @"interfaceCheckStatus":self.interfaceCheckStatus?:@"尚未开始"}];
    NSMutableString *text=[NSMutableString stringWithFormat:@"原始流：%@　融合：%@\n接近监测：%@　近远：%@\n标记：%lu\n\n",
        self.rawEnabled?@"开":@"关",self.motionEnabled?@"开":@"关",UIDevice.currentDevice.proximityMonitoringEnabled?@"开":@"不可用",
        UIDevice.currentDevice.proximityState?@"近":@"远",(unsigned long)self.marker];
    [text appendFormat:@"CoreMotion 状态：加速度 %@／陀螺仪 %@／融合 %@\n界面方向：%ld　设备方向：%ld\n自动启停检查：%@\n\n",
        self.motion.accelerometerActive?@"运行":@"停止",self.motion.gyroActive?@"运行":@"停止",
        self.motion.deviceMotionActive?@"运行":@"停止",(long)interface,(long)device,
        self.lifecycleDone?(self.lifecyclePassed?@"通过":@"未通过或中断"):
        [NSString stringWithFormat:@"第 %lu／7 步，无需移动手机",(unsigned long)self.lifecycleStage+1]];
    @synchronized(self.samples) {
        for(NSString *kind in @[@"accel",@"gyro",@"motion"]) {
            NSDictionary *s=self.samples[kind];
            if(s) { [self record:s]; [text appendFormat:@"%@：%@\n样本 %@，时间 %@，间隔 %@，错误 %@\n\n",kind,s[@"v"],s[@"n"],s[@"t"],s[@"dt"],s[@"error"]]; }
            else [text appendFormat:@"%@：等待样本\n\n",kind];
        }
    }
    [text appendFormat:@"界面旋转检查：%@\n",self.interfaceCheckStatus?:@"尚未开始"];
    self.output.text=text;
}
- (void)viewDidLoad
{
    [super viewDidLoad];self.view.backgroundColor=UIColor.systemBackgroundColor;
    self.motion=[CMMotionManager new];self.queue=[NSOperationQueue new];self.queue.maxConcurrentOperationCount=1;
    self.samples=[NSMutableDictionary new];
    self.motion.accelerometerUpdateInterval=self.motion.gyroUpdateInterval=self.motion.deviceMotionUpdateInterval=0.02;
    NSString *folder=NSSearchPathForDirectoriesInDomains(NSDocumentDirectory,NSUserDomainMask,YES).firstObject;
    NSString *path=[folder stringByAppendingPathComponent:@"nezha-sensors001.jsonl"];
    [NSFileManager.defaultManager createFileAtPath:path contents:[NSData data] attributes:nil];self.log=[NSFileHandle fileHandleForWritingAtPath:path];
    [self record:@{@"start":@1,@"uptime":@(NSProcessInfo.processInfo.systemUptime),@"accelAvailable":@(self.motion.accelerometerAvailable),
        @"gyroAvailable":@(self.motion.gyroAvailable),@"motionAvailable":@(self.motion.deviceMotionAvailable),
        @"referenceFrames":@([CMMotionManager availableAttitudeReferenceFrames]),@"pid":@(getpid()),@"candidate":@"sensors015"}];
    UILabel *title=[UILabel new];title.text=@"传感器测试015";title.font=[UIFont preferredFontForTextStyle:UIFontTextStyleTitle1];
    UILabel *hint=[UILabel new];hint.numberOfLines=0;hint.font=[UIFont preferredFontForTextStyle:UIFontTextStyleFootnote];hint.text=@"自动启停检查完成后，先按提示转动手机。界面旋转检查结束会恢复自动方向范围。无需标记。加速度与重力：g；角速度：rad/s。";
    self.directions=[UILabel new];self.directions.numberOfLines=0;
    self.directions.font=[UIFont preferredFontForTextStyle:UIFontTextStyleHeadline];
    NSMutableArray *views=[NSMutableArray arrayWithObjects:title,hint,self.directions,nil];
    NSArray *names=@[@"原始流开关",@"融合开关",@"标记",@"界面旋转检查"];
    SEL actions[]={@selector(toggleRaw),@selector(toggleMotion),@selector(mark),@selector(checkInterfaceRotation)};
    for(NSUInteger i=0;i<names.count;i++){UIButton *b=[UIButton buttonWithType:UIButtonTypeSystem];[b setTitle:names[i] forState:UIControlStateNormal];[b addTarget:self action:actions[i] forControlEvents:UIControlEventTouchUpInside];[views addObject:b];}
    self.output=[UITextView new];self.output.editable=NO;self.output.font=[UIFont monospacedSystemFontOfSize:14 weight:UIFontWeightRegular];[views addObject:self.output];
    UIStackView *stack=[[UIStackView alloc] initWithArrangedSubviews:views];stack.axis=UILayoutConstraintAxisVertical;stack.spacing=10;stack.translatesAutoresizingMaskIntoConstraints=NO;[self.view addSubview:stack];
    UILayoutGuide *safe=self.view.safeAreaLayoutGuide;
    [NSLayoutConstraint activateConstraints:@[[stack.leadingAnchor constraintEqualToAnchor:safe.leadingAnchor constant:16],
        [stack.trailingAnchor constraintEqualToAnchor:safe.trailingAnchor constant:-16],[stack.topAnchor constraintEqualToAnchor:safe.topAnchor constant:8],
        [stack.bottomAnchor constraintEqualToAnchor:safe.bottomAnchor constant:-8],[self.output.heightAnchor constraintGreaterThanOrEqualToConstant:80]]];
    [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(proximity:) name:UIDeviceProximityStateDidChangeNotification object:nil];
    [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(directionChanged:) name:UIDeviceOrientationDidChangeNotification object:nil];
    [UIDevice.currentDevice beginGeneratingDeviceOrientationNotifications];
    __weak SensorsController *weak=self;
    self.timer=[NSTimer timerWithTimeInterval:1 repeats:YES block:^(NSTimer *timer){(void)timer;[weak refresh];}];
    [NSRunLoop.mainRunLoop addTimer:self.timer forMode:NSRunLoopCommonModes];
}
- (void)dealloc
{
    [self.timer invalidate];[self.motion stopAccelerometerUpdates];[self.motion stopGyroUpdates];[self.motion stopDeviceMotionUpdates];
    [NSNotificationCenter.defaultCenter removeObserver:self];[UIDevice.currentDevice endGeneratingDeviceOrientationNotifications];UIDevice.currentDevice.proximityMonitoringEnabled=NO;[self.log closeFile];
}
@end

@interface SensorsSceneDelegate : UIResponder <UIWindowSceneDelegate>
@property(nonatomic,strong) UIWindow *window;
@end
@implementation SensorsSceneDelegate
- (void)scene:(UIScene *)scene willConnectToSession:(UISceneSession *)session options:(UISceneConnectionOptions *)options
{
    (void)session;(void)options;if(![scene isKindOfClass:UIWindowScene.class])return;
    self.window=[[UIWindow alloc] initWithWindowScene:(UIWindowScene *)scene];self.window.rootViewController=[SensorsController new];[self.window makeKeyAndVisible];
}
- (void)sceneDidEnterBackground:(UIScene *)scene
{
    (void)scene;SensorsController *c=(SensorsController *)self.window.rootViewController;
    [c cancelLifecycleCheck:@"background"];
    c.rawEnabled=NO;c.motionEnabled=NO;UIDevice.currentDevice.proximityMonitoringEnabled=NO;
    [c record:@{@"lifecycle":@"background",@"uptime":@(NSProcessInfo.processInfo.systemUptime)}];
}
- (void)sceneDidBecomeActive:(UIScene *)scene
{
    (void)scene;SensorsController *c=(SensorsController *)self.window.rootViewController;
    (void)c.view;
    [c record:@{@"lifecycle":@"active",@"uptime":@(NSProcessInfo.processInfo.systemUptime)}];
    c.rawEnabled=YES;c.motionEnabled=YES;UIDevice.currentDevice.proximityMonitoringEnabled=YES;
    [c startLifecycleCheck];
    if(!c.interfaceCheckRunning)[c restoreAutomaticInterface];
}
@end
@interface SensorsAppDelegate : UIResponder <UIApplicationDelegate>
@end
@implementation SensorsAppDelegate
@end
int main(int argc,char **argv){@autoreleasepool{return UIApplicationMain(argc,argv,nil,NSStringFromClass(SensorsAppDelegate.class));}}
