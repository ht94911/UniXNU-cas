// SPDX-License-Identifier: BSD-2-Clause-Patent
#import <UIKit/UIKit.h>
#import <CoreMotion/CoreMotion.h>
#include <unistd.h>

@interface SensorsController : UIViewController
@property(nonatomic,strong) CMMotionManager *motion;
@property(nonatomic,strong) NSOperationQueue *queue;
@property(nonatomic,strong) UITextView *output;
@property(nonatomic,strong) NSFileHandle *log;
@property(nonatomic,strong) NSTimer *timer;
@property(nonatomic,strong) NSMutableDictionary *samples;
@property(nonatomic) BOOL rawEnabled,motionEnabled;
@property(nonatomic) NSUInteger marker;
@property(nonatomic) NSUInteger refreshCount;
@property(nonatomic) NSUInteger lifecycleStage,lifecycleTicks;
@property(nonatomic) BOOL lifecycleStarted,lifecycleDone,lifecyclePassed;
@property(nonatomic,strong) NSDictionary *lifecycleCheckpoint;
@end

@implementation SensorsController
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
- (void)refresh
{
    self.refreshCount++;
    [self stepLifecycleCheck];
    [self record:@{@"status":@1,@"refresh":@(self.refreshCount),@"uptime":@(NSProcessInfo.processInfo.systemUptime),
        @"applicationState":@(UIApplication.sharedApplication.applicationState),@"accelAvailable":@(self.motion.accelerometerAvailable),
        @"gyroAvailable":@(self.motion.gyroAvailable),@"motionAvailable":@(self.motion.deviceMotionAvailable),
        @"accelActive":@(self.motion.accelerometerActive),@"gyroActive":@(self.motion.gyroActive),
        @"motionActive":@(self.motion.deviceMotionActive),@"proximityEnabled":@(UIDevice.currentDevice.proximityMonitoringEnabled),
        @"deviceOrientation":@(UIDevice.currentDevice.orientation),
        @"interfaceOrientation":@(self.view.window.windowScene.effectiveGeometry.interfaceOrientation)}];
    NSMutableString *text=[NSMutableString stringWithFormat:@"原始流：%@　融合：%@\n接近监测：%@　近远：%@\n标记：%lu\n\n",
        self.rawEnabled?@"开":@"关",self.motionEnabled?@"开":@"关",UIDevice.currentDevice.proximityMonitoringEnabled?@"开":@"不可用",
        UIDevice.currentDevice.proximityState?@"近":@"远",(unsigned long)self.marker];
    [text appendFormat:@"CoreMotion 状态：加速度 %@／陀螺仪 %@／融合 %@\n界面方向：%ld　设备方向：%ld\n自动启停检查：%@\n\n",
        self.motion.accelerometerActive?@"运行":@"停止",self.motion.gyroActive?@"运行":@"停止",
        self.motion.deviceMotionActive?@"运行":@"停止",(long)self.view.window.windowScene.effectiveGeometry.interfaceOrientation,
        (long)UIDevice.currentDevice.orientation,self.lifecycleDone?(self.lifecyclePassed?@"通过":@"未通过或中断"):
        [NSString stringWithFormat:@"第 %lu／7 步，无需移动手机",(unsigned long)self.lifecycleStage+1]];
    @synchronized(self.samples) {
        for(NSString *kind in @[@"accel",@"gyro",@"motion"]) {
            NSDictionary *s=self.samples[kind];
            if(s) { [self record:s]; [text appendFormat:@"%@：%@\n样本 %@，时间 %@，间隔 %@，错误 %@\n\n",kind,s[@"v"],s[@"n"],s[@"t"],s[@"dt"],s[@"error"]]; }
            else [text appendFormat:@"%@：等待样本\n\n",kind];
        }
    }
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
        @"referenceFrames":@([CMMotionManager availableAttitudeReferenceFrames]),@"pid":@(getpid()),@"candidate":@"sensors008"}];
    UILabel *title=[UILabel new];title.text=@"传感器测试008";title.font=[UIFont preferredFontForTextStyle:UIFontTextStyleTitle1];
    UILabel *hint=[UILabel new];hint.numberOfLines=0;hint.font=[UIFont preferredFontForTextStyle:UIFontTextStyleFootnote];hint.text=@"启动后自动检查启停约70秒，无需移动手机。人工测试时先完成动作或摆好并静置，最后点一次“标记”，记录此前样本。姿势数据只作功能参考，绝不用于校准。加速度与重力：g；角速度：rad/s。";
    NSMutableArray *views=[NSMutableArray arrayWithObjects:title,hint,nil];
    NSArray *names=@[@"原始流开关",@"融合开关",@"标记"];
    SEL actions[]={@selector(toggleRaw),@selector(toggleMotion),@selector(mark)};
    for(NSUInteger i=0;i<names.count;i++){UIButton *b=[UIButton buttonWithType:UIButtonTypeSystem];[b setTitle:names[i] forState:UIControlStateNormal];[b addTarget:self action:actions[i] forControlEvents:UIControlEventTouchUpInside];[views addObject:b];}
    self.output=[UITextView new];self.output.editable=NO;self.output.font=[UIFont monospacedSystemFontOfSize:14 weight:UIFontWeightRegular];[views addObject:self.output];
    UIStackView *stack=[[UIStackView alloc] initWithArrangedSubviews:views];stack.axis=UILayoutConstraintAxisVertical;stack.spacing=10;stack.translatesAutoresizingMaskIntoConstraints=NO;[self.view addSubview:stack];
    UILayoutGuide *safe=self.view.safeAreaLayoutGuide;
    [NSLayoutConstraint activateConstraints:@[[stack.leadingAnchor constraintEqualToAnchor:safe.leadingAnchor constant:16],
        [stack.trailingAnchor constraintEqualToAnchor:safe.trailingAnchor constant:-16],[stack.topAnchor constraintEqualToAnchor:safe.topAnchor constant:8],
        [stack.bottomAnchor constraintEqualToAnchor:safe.bottomAnchor constant:-8],[self.output.heightAnchor constraintGreaterThanOrEqualToConstant:80]]];
    [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(proximity:) name:UIDeviceProximityStateDidChangeNotification object:nil];
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
}
@end
@interface SensorsAppDelegate : UIResponder <UIApplicationDelegate>
@end
@implementation SensorsAppDelegate
@end
int main(int argc,char **argv){@autoreleasepool{return UIApplicationMain(argc,argv,nil,NSStringFromClass(SensorsAppDelegate.class));}}
