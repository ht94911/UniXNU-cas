// Test fixture for installation, launch and Data persistence on iOS.
#import <UIKit/UIKit.h>

@interface ProbeScene : UIResponder <UIWindowSceneDelegate>
@property(nonatomic, strong) UIWindow *window;
@end

@implementation ProbeScene
- (void)scene:(UIScene *)scene willConnectToSession:(UISceneSession *)session
       options:(UISceneConnectionOptions *)options
{
    (void)session;
    (void)options;
    if (![scene isKindOfClass:[UIWindowScene class]]) return;
    NSBundle *bundle = NSBundle.mainBundle;
    NSUserDefaults *defaults = NSUserDefaults.standardUserDefaults;
    NSInteger launches = [defaults integerForKey:@"AppSyncProbeLaunches"] + 1;
    [defaults setInteger:launches forKey:@"AppSyncProbeLaunches"];
    UIViewController *view = [[UIViewController alloc] init];
    view.view.backgroundColor = UIColor.systemBackgroundColor;
    UILabel *label = [[UILabel alloc] init];
    label.textColor = UIColor.labelColor;
    label.numberOfLines = 0;
    label.textAlignment = NSTextAlignmentCenter;
    label.font = [UIFont preferredFontForTextStyle:UIFontTextStyleTitle2];
    label.text = [NSString stringWithFormat:@"AppSync Probe\n%@\nVersion %@\nLaunch %ld",
        [bundle objectForInfoDictionaryKey:@"CFBundleDisplayName"],
        [bundle objectForInfoDictionaryKey:@"CFBundleVersion"], (long)launches];
    label.translatesAutoresizingMaskIntoConstraints = NO;
    [view.view addSubview:label];
    [NSLayoutConstraint activateConstraints:@[
        [label.centerYAnchor constraintEqualToAnchor:view.view.centerYAnchor],
        [label.leadingAnchor constraintEqualToAnchor:view.view.safeAreaLayoutGuide.leadingAnchor constant:16],
        [label.trailingAnchor constraintEqualToAnchor:view.view.safeAreaLayoutGuide.trailingAnchor constant:-16],
    ]];
    self.window = [[UIWindow alloc] initWithWindowScene:(UIWindowScene *)scene];
    self.window.rootViewController = view;
    [self.window makeKeyAndVisible];
}
@end

@interface ProbeApp : UIResponder <UIApplicationDelegate>
@end
@implementation ProbeApp
- (UISceneConfiguration *)application:(UIApplication *)application
    configurationForConnectingSceneSession:(UISceneSession *)session
    options:(UISceneConnectionOptions *)options
{
    (void)application;
    (void)options;
    UISceneConfiguration *cfg = [[UISceneConfiguration alloc] initWithName:@"AppSync Probe"
                                                              sessionRole:session.role];
    cfg.delegateClass = ProbeScene.class;
    return cfg;
}
@end

int main(int argc, char *argv[])
{
    @autoreleasepool {
        return UIApplicationMain(argc, argv, nil, NSStringFromClass(ProbeApp.class));
    }
}
