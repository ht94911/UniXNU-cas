#import <UIKit/UIKit.h>
#import "AccountKeyItemProbe.h"

@interface AA27Controller : UIViewController
@property(nonatomic, strong) UITextView *output;
@property(nonatomic, strong) UIButton *runButton;
@property(nonatomic, strong) UIButton *clearButton;
@property(nonatomic, strong) dispatch_queue_t worker;
@property(nonatomic) BOOL busy;
@end

@implementation AA27Controller
- (void)viewDidLoad
{
    [super viewDidLoad];
    self.view.backgroundColor = UIColor.systemBackgroundColor;
    self.worker = dispatch_queue_create("org.unixnu.nezha.account27.test", DISPATCH_QUEUE_SERIAL);

    UILabel *title = [UILabel new];
    title.text = @"AA27 K1";
    title.font = [UIFont preferredFontForTextStyle:UIFontTextStyleTitle1];
    UILabel *detail = [UILabel new];
    detail.numberOfLines = 0;
    detail.text = @"Check two public test items in this app's own group. After a normal restart, open this app and run again to check the saved item.";
    detail.font = [UIFont preferredFontForTextStyle:UIFontTextStyleBody];
    self.runButton = [UIButton buttonWithType:UIButtonTypeSystem];
    [self.runButton setTitle:@"Run check" forState:UIControlStateNormal];
    [self.runButton addTarget:self action:@selector(runCheck) forControlEvents:UIControlEventTouchUpInside];
    self.clearButton = [UIButton buttonWithType:UIButtonTypeSystem];
    [self.clearButton setTitle:@"Remove test items" forState:UIControlStateNormal];
    [self.clearButton addTarget:self action:@selector(clearItems) forControlEvents:UIControlEventTouchUpInside];
    self.output = [UITextView new];
    self.output.editable = NO;
    self.output.font = [UIFont monospacedSystemFontOfSize:14 weight:UIFontWeightRegular];
    self.output.text = @"Ready. No account details are requested.";
    self.output.accessibilityLabel = @"AA27 test result";

    UIStackView *stack = [[UIStackView alloc] initWithArrangedSubviews:@[
        title, detail, self.runButton, self.clearButton, self.output]];
    stack.axis = UILayoutConstraintAxisVertical;
    stack.spacing = 12;
    stack.translatesAutoresizingMaskIntoConstraints = NO;
    [self.view addSubview:stack];
    UILayoutGuide *safe = self.view.safeAreaLayoutGuide;
    [NSLayoutConstraint activateConstraints:@[
        [stack.leadingAnchor constraintEqualToAnchor:safe.leadingAnchor constant:20],
        [stack.trailingAnchor constraintEqualToAnchor:safe.trailingAnchor constant:-20],
        [stack.topAnchor constraintEqualToAnchor:safe.topAnchor constant:12],
        [stack.bottomAnchor constraintEqualToAnchor:safe.bottomAnchor constant:-12],
        [self.output.heightAnchor constraintGreaterThanOrEqualToConstant:180]]];
}

- (void)runCheck { [self perform:@"auto"]; }
- (void)clearItems { [self perform:@"cleanup"]; }

- (void)perform:(NSString *)operation
{
    if (self.busy) return;
    self.busy = YES;
    self.runButton.enabled = NO;
    self.clearButton.enabled = NO;
    self.output.text = @"Running the original SecItem calls…";
    dispatch_async(self.worker, ^{
        NSDictionary *result = AA27PerformProbe(operation, ^(NSDictionary *snapshot) {
            NSData *encoded = [NSJSONSerialization dataWithJSONObject:snapshot
                options:NSJSONWritingPrettyPrinted | NSJSONWritingSortedKeys error:NULL];
            NSString *progress = encoded ? [[NSString alloc] initWithData:encoded encoding:NSUTF8StringEncoding]
                                         : @"Step result could not be encoded.";
            dispatch_async(dispatch_get_main_queue(), ^{
                self.output.text = progress;
            });
        });
        NSData *data = [NSJSONSerialization dataWithJSONObject:result
            options:NSJSONWritingPrettyPrinted | NSJSONWritingSortedKeys error:NULL];
        NSString *text = data ? [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding]
                             : @"Result could not be encoded.";
        dispatch_async(dispatch_get_main_queue(), ^{
            self.output.text = text;
            self.busy = NO;
            self.runButton.enabled = YES;
            self.clearButton.enabled = YES;
        });
    });
}
@end

@interface AA27SceneDelegate : UIResponder <UIWindowSceneDelegate>
@property(nonatomic, strong) UIWindow *window;
@end

@implementation AA27SceneDelegate
- (void)scene:(UIScene *)scene willConnectToSession:(UISceneSession *)session
      options:(UISceneConnectionOptions *)connectionOptions
{
    (void)session;
    (void)connectionOptions;
    if (![scene isKindOfClass:UIWindowScene.class]) return;
    self.window = [[UIWindow alloc] initWithWindowScene:(UIWindowScene *)scene];
    self.window.rootViewController = [AA27Controller new];
    [self.window makeKeyAndVisible];
}
@end

@interface AA27AppDelegate : UIResponder <UIApplicationDelegate>
@end
@implementation AA27AppDelegate
@end

int main(int argc, char **argv)
{
    @autoreleasepool {
        return UIApplicationMain(argc, argv, nil, NSStringFromClass(AA27AppDelegate.class));
    }
}
