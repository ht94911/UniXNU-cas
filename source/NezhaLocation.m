/* SPDX-License-Identifier: BSD-2-Clause */
/* Foreground Core Location consumer. It requests normal WhenInUse permission
 * and records only locations delivered by CLLocationManager. No private
 * positioning API, fixed coordinate, simulated source or TCC edit. */
#import <UIKit/UIKit.h>
#import <CoreLocation/CoreLocation.h>
#include <sys/sysctl.h>

@interface NezhaLocationController : UIViewController <CLLocationManagerDelegate>
@property(nonatomic,strong) CLLocationManager *manager;
@property(nonatomic,strong) UILabel *status;
@property(nonatomic,strong) UILabel *position;
@property(nonatomic,strong) UIButton *startButton;
@property(nonatomic,strong) UIButton *stopButton;
@property(nonatomic,strong) NSMutableArray *records;
@property(nonatomic,copy) NSString *bootUUID;
@property(nonatomic) BOOL requested;
@property(nonatomic) BOOL running;
@end

@implementation NezhaLocationController
- (void)viewDidLoad
{
    [super viewDidLoad];
    self.view.backgroundColor=UIColor.systemBackgroundColor;
    self.title=@"卫星定位测试";
    self.manager=[CLLocationManager new];self.manager.delegate=self;
    self.manager.desiredAccuracy=kCLLocationAccuracyBest;self.manager.distanceFilter=kCLDistanceFilterNone;
    self.manager.pausesLocationUpdatesAutomatically=NO;
    self.records=[NSMutableArray array];
    char uuid[128]={0};size_t bytes=sizeof uuid;
    self.bootUUID=sysctlbyname("kern.bootsessionuuid",uuid,&bytes,NULL,0)==0 ? @(uuid):@"unavailable";
    self.status=[UILabel new];self.status.numberOfLines=0;self.status.font=[UIFont preferredFontForTextStyle:UIFontTextStyleHeadline];
    self.status.text=@"点击开始，按系统提示允许定位。";
    self.position=[UILabel new];self.position.numberOfLines=0;self.position.font=[UIFont monospacedSystemFontOfSize:16 weight:UIFontWeightRegular];
    self.position.text=@"等待新的系统位置";
    self.startButton=[UIButton buttonWithType:UIButtonTypeSystem];[self.startButton setTitle:@"开始定位" forState:UIControlStateNormal];
    [self.startButton addTarget:self action:@selector(begin) forControlEvents:UIControlEventTouchUpInside];
    self.stopButton=[UIButton buttonWithType:UIButtonTypeSystem];[self.stopButton setTitle:@"停止定位" forState:UIControlStateNormal];
    [self.stopButton addTarget:self action:@selector(end) forControlEvents:UIControlEventTouchUpInside];
    UIStackView *stack=[[UIStackView alloc]initWithArrangedSubviews:@[self.status,self.position,self.startButton,self.stopButton]];
    stack.axis=UILayoutConstraintAxisVertical;stack.spacing=24;stack.translatesAutoresizingMaskIntoConstraints=NO;
    [self.view addSubview:stack];UILayoutGuide *safe=self.view.safeAreaLayoutGuide;
    [NSLayoutConstraint activateConstraints:@[[stack.leadingAnchor constraintEqualToAnchor:safe.leadingAnchor constant:24],
        [stack.trailingAnchor constraintEqualToAnchor:safe.trailingAnchor constant:-24],
        [stack.topAnchor constraintEqualToAnchor:safe.topAnchor constant:28]]];
}
- (NSString *)authorization
{
    switch(self.manager.authorizationStatus){
    case kCLAuthorizationStatusAuthorizedWhenInUse:return @"authorizedWhenInUse";
    case kCLAuthorizationStatusAuthorizedAlways:return @"authorizedAlways";
    case kCLAuthorizationStatusDenied:return @"denied";
    case kCLAuthorizationStatusRestricted:return @"restricted";
    default:return @"notDetermined";
    }
}
- (void)save:(NSDictionary *)event
{
    [self.records addObject:event];if(self.records.count>256)[self.records removeObjectAtIndex:0];
    NSError *error=nil;
    NSString *documents=NSSearchPathForDirectoriesInDomains(NSDocumentDirectory,NSUserDomainMask,YES).firstObject;
    NSData *body=[NSJSONSerialization dataWithJSONObject:@{@"provider":@"CoreLocation",@"boot_session_uuid":self.bootUUID,
        @"authorization":[self authorization],@"accuracy_authorization":self.manager.accuracyAuthorization==CLAccuracyAuthorizationFullAccuracy ? @"fullAccuracy":@"reducedAccuracy",
        @"events":self.records} options:NSJSONWritingPrettyPrinted error:&error];
    if(body&&documents)[body writeToFile:[documents stringByAppendingPathComponent:@"corelocation.json"] options:NSDataWritingAtomic error:&error];
    if(error)NSLog(@"NezhaLocation evidence write: %@",error);
}
- (void)begin
{
    self.requested=YES;
    if(self.manager.authorizationStatus==kCLAuthorizationStatusNotDetermined){
        self.status.text=@"请在系统提示中允许定位并开启精确位置。";
        [self.manager requestWhenInUseAuthorization];return;
    }
    if(self.manager.authorizationStatus!=kCLAuthorizationStatusAuthorizedWhenInUse&&
       self.manager.authorizationStatus!=kCLAuthorizationStatusAuthorizedAlways){
        self.status.text=@"定位未授权，请在设置中允许此应用定位。";return;
    }
    if(!self.running){self.running=YES;[self.manager startUpdatingLocation];
        [self save:@{@"event":@"start",@"received_unix_ms":@((long long)(NSDate.date.timeIntervalSince1970*1000))}];}
    self.status.text=@"定位中，等待新的系统位置。";
}
- (void)end
{
    self.requested=NO;[self.manager stopUpdatingLocation];self.running=NO;self.status.text=@"定位已停止";
    [self save:@{@"event":@"stop",@"received_unix_ms":@((long long)(NSDate.date.timeIntervalSince1970*1000))}];
}
- (void)locationManagerDidChangeAuthorization:(CLLocationManager *)manager
{
    (void)manager;if(self.requested&&self.manager.authorizationStatus!=kCLAuthorizationStatusNotDetermined)[self begin];
}
- (void)locationManager:(CLLocationManager *)manager didUpdateLocations:(NSArray<CLLocation *> *)locations
{
    (void)manager;
    for(CLLocation *location in locations){
        CLLocationSourceInformation *source=location.sourceInformation;
        NSDictionary *row=@{@"event":@"location",@"received_unix_ms":@((long long)(NSDate.date.timeIntervalSince1970*1000)),
            @"location":@{@"latitude":@(location.coordinate.latitude),@"longitude":@(location.coordinate.longitude),
                @"horizontal_accuracy":@(location.horizontalAccuracy),@"altitude":@(location.altitude),
                @"vertical_accuracy":@(location.verticalAccuracy),@"speed":@(location.speed),@"course":@(location.course),
                @"timestamp_unix_ms":@((long long)(location.timestamp.timeIntervalSince1970*1000)),
                @"simulated_by_software":source ? @(source.isSimulatedBySoftware):(id)NSNull.null,
                @"produced_by_accessory":source ? @(source.isProducedByAccessory):(id)NSNull.null}};
        [self save:row];
        self.position.text=[NSString stringWithFormat:@"纬度 %.7f\n经度 %.7f\n水平精度 %.1f 米\n时间 %@",
            location.coordinate.latitude,location.coordinate.longitude,location.horizontalAccuracy,location.timestamp];
        self.status.text=@"已收到系统位置；卫星来源由接收机记录核对。";
    }
}
- (void)locationManager:(CLLocationManager *)manager didFailWithError:(NSError *)error
{
    (void)manager;self.status.text=[NSString stringWithFormat:@"定位状态：%@",error.localizedDescription];
    [self save:@{@"event":@"error",@"domain":error.domain,@"code":@(error.code),@"received_unix_ms":@((long long)(NSDate.date.timeIntervalSince1970*1000))}];
}
@end

@interface NezhaLocationApp : UIResponder <UIApplicationDelegate>
@property(nonatomic,strong) UIWindow *window;
@end
@implementation NezhaLocationApp
- (BOOL)application:(UIApplication *)app didFinishLaunchingWithOptions:(NSDictionary *)options
{
    (void)app;(void)options;self.window=[[UIWindow alloc]initWithFrame:UIScreen.mainScreen.bounds];
    self.window.rootViewController=[[UINavigationController alloc]initWithRootViewController:[NezhaLocationController new]];
    [self.window makeKeyAndVisible];return YES;
}
- (void)applicationDidEnterBackground:(UIApplication *)app
{
    (void)app;UINavigationController *nav=(UINavigationController *)self.window.rootViewController;
    [(NezhaLocationController *)nav.topViewController end];
}
@end
int main(int argc,char **argv)
{
    @autoreleasepool{return UIApplicationMain(argc,argv,nil,NSStringFromClass(NezhaLocationApp.class));}
}
