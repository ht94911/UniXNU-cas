// SPDX-License-Identifier: BSD-2-Clause-Patent
// Ordinary UIKit/CoreHaptics requests in a foreground diagnostic application.
#import <UIKit/UIKit.h>
#import <CoreHaptics/CoreHaptics.h>

@interface HapticsController : UIViewController
@property(nonatomic,strong) UILabel *status;
@property(nonatomic,strong) NSFileHandle *log;
@property(nonatomic,strong) CHHapticEngine *engine;
@property(nonatomic,strong) id<CHHapticPatternPlayer> player;
@property(nonatomic) NSUInteger events;
@property(nonatomic) BOOL automaticTestStarted;
@end

@implementation HapticsController
- (void)record:(NSString *)event success:(BOOL)success error:(NSError *)error
{
    if(self.events++ >= 256)return;
    NSDictionary *row=@{@"event":event,@"uptime":@(NSProcessInfo.processInfo.systemUptime),
        @"success":@(success),@"error":@(error.code),@"errorDomain":error.domain ?: @"",
        @"applicationState":@(UIApplication.sharedApplication.applicationState),
        @"mechanicalVerified":@NO};
    NSData *data=[NSJSONSerialization dataWithJSONObject:row options:NSJSONWritingSortedKeys error:NULL];
    @try { [self.log writeData:data]; [self.log writeData:[@"\n" dataUsingEncoding:NSUTF8StringEncoding]]; }
    @catch(NSException *exception) { NSLog(@"Haptics diagnostic log %@",exception.name); }
    self.status.text=[NSString stringWithFormat:@"%@：%@",event,success?@"已发出":@"失败"];
}
- (void)selection
{
    UISelectionFeedbackGenerator *generator=[UISelectionFeedbackGenerator new];
    [generator prepare];[generator selectionChanged];
    [self record:@"UIKit selection" success:YES error:nil];
}
- (void)impact
{
    UIImpactFeedbackGenerator *generator=[[UIImpactFeedbackGenerator alloc] initWithStyle:UIImpactFeedbackStyleMedium];
    [generator prepare];[generator impactOccurredWithIntensity:0.5];
    [self record:@"UIKit impact medium 50%" success:YES error:nil];
}
- (void)notification
{
    UINotificationFeedbackGenerator *generator=[UINotificationFeedbackGenerator new];
    [generator prepare];[generator notificationOccurred:UINotificationFeedbackTypeSuccess];
    [self record:@"UIKit notification success" success:YES error:nil];
}
- (void)continuous
{
    NSError *error=nil;
    if(!self.engine) {
        self.engine=[[CHHapticEngine alloc] initAndReturnError:&error];
        [self record:@"CoreHaptics engine" success:self.engine!=nil error:error];
        if(!self.engine)return;
        self.engine.playsHapticsOnly=YES;
    }
    BOOL started=[self.engine startAndReturnError:&error];
    [self record:@"CoreHaptics start" success:started error:error];if(!started)return;
    NSArray *parameters=@[
        [[CHHapticEventParameter alloc] initWithParameterID:CHHapticEventParameterIDHapticIntensity value:0.5],
        [[CHHapticEventParameter alloc] initWithParameterID:CHHapticEventParameterIDHapticSharpness value:0.5]];
    CHHapticEvent *event=[[CHHapticEvent alloc] initWithEventType:CHHapticEventTypeHapticContinuous
        parameters:parameters relativeTime:0 duration:0.15];
    CHHapticPattern *pattern=[[CHHapticPattern alloc] initWithEvents:@[event] parameters:@[] error:&error];
    self.player=pattern?[self.engine createPlayerWithPattern:pattern error:&error]:nil;
    [self record:@"CoreHaptics pattern" success:self.player!=nil error:error];if(!self.player)return;
    started=[self.player startAtTime:CHHapticTimeImmediate error:&error];
    [self record:@"CoreHaptics 150ms 50%" success:started error:error];
}
- (void)stop
{
    NSError *error=nil;
    if(self.player) {
        BOOL stopped=[self.player stopAtTime:CHHapticTimeImmediate error:&error];
        [self record:@"CoreHaptics cancel" success:stopped error:error];self.player=nil;
    }
    if(self.engine) {
        __weak HapticsController *weak=self;
        [self.engine stopWithCompletionHandler:^(NSError *stopError) {
            dispatch_async(dispatch_get_main_queue(),^{[weak record:@"CoreHaptics stop callback" success:stopError==nil error:stopError];});
        }];
    }
}
- (void)viewDidLoad
{
    [super viewDidLoad];self.view.backgroundColor=UIColor.systemBackgroundColor;
    NSString *folder=NSSearchPathForDirectoriesInDomains(NSDocumentDirectory,NSUserDomainMask,YES).firstObject;
    NSString *path=[folder stringByAppendingPathComponent:@"nezha-haptics-ui001.jsonl"];
    [NSFileManager.defaultManager createFileAtPath:path contents:[NSData data] attributes:nil];
    self.log=[NSFileHandle fileHandleForWritingAtPath:path];
    UILabel *title=[UILabel new];title.text=@"触觉测试";title.font=[UIFont preferredFontForTextStyle:UIFontTextStyleTitle1];
    UILabel *hint=[UILabel new];hint.numberOfLines=0;
    hint.text=@"打开后会依次测试选择、轻触、通知和一次短触觉。各项相隔一秒，随后停止。也可点按钮分别测试。";
    self.status=[UILabel new];self.status.numberOfLines=0;self.status.text=@"等待测试";
    NSMutableArray *views=[NSMutableArray arrayWithObjects:title,hint,self.status,nil];
    NSArray *names=@[@"选择反馈",@"轻触反馈",@"通知反馈",@"短连续触觉",@"停止"];
    SEL actions[]={@selector(selection),@selector(impact),@selector(notification),@selector(continuous),@selector(stop)};
    for(NSUInteger i=0;i<names.count;i++) {
        UIButton *button=[UIButton buttonWithType:UIButtonTypeSystem];
        [button setTitle:names[i] forState:UIControlStateNormal];
        [button addTarget:self action:actions[i] forControlEvents:UIControlEventTouchUpInside];[views addObject:button];
    }
    UIStackView *stack=[[UIStackView alloc] initWithArrangedSubviews:views];
    stack.axis=UILayoutConstraintAxisVertical;stack.spacing=18;stack.translatesAutoresizingMaskIntoConstraints=NO;
    [self.view addSubview:stack];UILayoutGuide *safe=self.view.safeAreaLayoutGuide;
    [NSLayoutConstraint activateConstraints:@[[stack.leadingAnchor constraintEqualToAnchor:safe.leadingAnchor constant:20],
        [stack.trailingAnchor constraintEqualToAnchor:safe.trailingAnchor constant:-20],
        [stack.topAnchor constraintEqualToAnchor:safe.topAnchor constant:20]]];
}
- (void)viewDidAppear:(BOOL)animated
{
    [super viewDidAppear:animated];if(self.automaticTestStarted)return;self.automaticTestStarted=YES;
    [self record:@"foreground test start" success:YES error:nil];
    __weak HapticsController *weak=self;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW,2*NSEC_PER_SEC),dispatch_get_main_queue(),^{[weak selection];});
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW,3*NSEC_PER_SEC),dispatch_get_main_queue(),^{[weak impact];});
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW,4*NSEC_PER_SEC),dispatch_get_main_queue(),^{[weak notification];});
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW,5*NSEC_PER_SEC),dispatch_get_main_queue(),^{[weak continuous];});
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW,6*NSEC_PER_SEC),dispatch_get_main_queue(),^{[weak stop];});
}
- (void)viewWillDisappear:(BOOL)animated { [self stop];[super viewWillDisappear:animated]; }
- (void)dealloc { [self.log closeFile]; }
@end

@interface HapticsSceneDelegate : UIResponder <UIWindowSceneDelegate>
@property(nonatomic,strong) UIWindow *window;
@end
@implementation HapticsSceneDelegate
- (void)scene:(UIScene *)scene willConnectToSession:(UISceneSession *)session options:(UISceneConnectionOptions *)options
{
    (void)session;(void)options;if(![scene isKindOfClass:UIWindowScene.class])return;
    self.window=[[UIWindow alloc] initWithWindowScene:(UIWindowScene *)scene];
    self.window.rootViewController=[HapticsController new];[self.window makeKeyAndVisible];
}
@end
@interface HapticsAppDelegate : UIResponder <UIApplicationDelegate>
@end
@implementation HapticsAppDelegate
@end
int main(int argc,char **argv)
{
    @autoreleasepool { return UIApplicationMain(argc,argv,nil,NSStringFromClass(HapticsAppDelegate.class)); }
}
