/** @file
 * @brief Native macOS wrapper for launching and presenting the CLI DMA demo. */
#import <Cocoa/Cocoa.h>

/** Owns the demo window and supervises the bundled FreeRTOS command-line engine. */
@interface UartDemoAppDelegate : NSObject <NSApplicationDelegate>
/** Main application window. */
@property(nonatomic, strong) NSWindow *window;
/** Read-only monospaced engine output. */
@property(nonatomic, strong) NSTextView *logView;
/** Current execution status. */
@property(nonatomic, strong) NSTextField *statusLabel;
/** Button that starts a round-trip run. */
@property(nonatomic, strong) NSButton *startButton;
/** Indeterminate activity indicator. */
@property(nonatomic, strong) NSProgressIndicator *progress;
/** Currently running bundled helper task. */
@property(nonatomic, strong) NSTask *demoTask;
@end

@implementation UartDemoAppDelegate

/** Append engine output to the scrolling log view. */
- (void)appendLog:(NSString *)text {
    if (text.length == 0U) {
        return;
    }

    NSTextStorage *storage = self.logView.textStorage;
    NSDictionary<NSAttributedStringKey, id> *attributes = @{
        NSFontAttributeName: [NSFont monospacedSystemFontOfSize:12.0
                                                       weight:NSFontWeightRegular],
        NSForegroundColorAttributeName: NSColor.labelColor
    };
    [storage appendAttributedString:[[NSAttributedString alloc] initWithString:text
                                                                    attributes:attributes]];
    [self.logView scrollRangeToVisible:NSMakeRange(storage.length, 0U)];
}

/** Update controls and animation for running or idle state. */
- (void)setRunning:(BOOL)running {
    self.startButton.enabled = !running;
    if (running) {
        self.statusLabel.stringValue = @"Running real FreeRTOS tasks…";
        self.statusLabel.textColor = NSColor.systemBlueColor;
        [self.progress startAnimation:nil];
    } else {
        [self.progress stopAnimation:nil];
    }
}

/** Launch the bundled CLI demo and asynchronously consume its output. */
- (void)startDemo:(id)sender {
    (void)sender;
    if (self.demoTask.running) {
        return;
    }

    self.logView.string = @"";
    [self appendLog:@"Launching bundled FreeRTOS UART DMA engine…\n\n"];
    [self setRunning:YES];

    NSURL *helperUrl = [NSBundle.mainBundle URLForResource:@"uart_dma_demo_cli"
                                             withExtension:nil];
    if (helperUrl == nil) {
        self.statusLabel.stringValue = @"Bundled demo engine not found";
        self.statusLabel.textColor = NSColor.systemRedColor;
        [self setRunning:NO];
        return;
    }

    NSTask *task = [[NSTask alloc] init];
    NSPipe *outputPipe = [NSPipe pipe];
    task.executableURL = helperUrl;
    task.standardOutput = outputPipe;
    task.standardError = outputPipe;
    self.demoTask = task;

    __weak typeof(self) weakSelf = self;
    outputPipe.fileHandleForReading.readabilityHandler = ^(NSFileHandle *handle) {
        NSData *data = handle.availableData;
        if (data.length == 0U) {
            return;
        }
        NSString *text = [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding];
        dispatch_async(dispatch_get_main_queue(), ^{
            [weakSelf appendLog:text ?: @""];
        });
    };

    task.terminationHandler = ^(NSTask *finishedTask) {
        outputPipe.fileHandleForReading.readabilityHandler = nil;
        dispatch_async(dispatch_get_main_queue(), ^{
            UartDemoAppDelegate *strongSelf = weakSelf;
            [strongSelf setRunning:NO];
            if (finishedTask.terminationStatus == 0) {
                strongSelf.statusLabel.stringValue = @"All 4 DMA round trips passed";
                strongSelf.statusLabel.textColor = NSColor.systemGreenColor;
            } else {
                strongSelf.statusLabel.stringValue = [NSString stringWithFormat:
                    @"Demo failed with status %d", finishedTask.terminationStatus];
                strongSelf.statusLabel.textColor = NSColor.systemRedColor;
            }
            strongSelf.demoTask = nil;
        });
    };

    NSError *error = nil;
    if (![task launchAndReturnError:&error]) {
        outputPipe.fileHandleForReading.readabilityHandler = nil;
        self.demoTask = nil;
        [self setRunning:NO];
        self.statusLabel.stringValue = error.localizedDescription;
        self.statusLabel.textColor = NSColor.systemRedColor;
    }
}

/** Construct and display the Cocoa interface after application launch. */
- (void)applicationDidFinishLaunching:(NSNotification *)notification {
    (void)notification;
    const NSRect frame = NSMakeRect(0.0, 0.0, 760.0, 520.0);
    self.window = [[NSWindow alloc]
        initWithContentRect:frame
                  styleMask:NSWindowStyleMaskTitled |
                            NSWindowStyleMaskClosable |
                            NSWindowStyleMaskMiniaturizable |
                            NSWindowStyleMaskResizable
                    backing:NSBackingStoreBuffered
                      defer:NO];
    self.window.title = @"UART DMA + FreeRTOS Demo";
    self.window.minSize = NSMakeSize(600.0, 400.0);

    NSView *content = self.window.contentView;
    NSTextField *title = [NSTextField labelWithString:@"FreeRTOS UART DMA Round Trip"];
    title.font = [NSFont systemFontOfSize:22.0 weight:NSFontWeightSemibold];
    title.translatesAutoresizingMaskIntoConstraints = NO;

    NSTextField *subtitle = [NSTextField labelWithString:
        @"HCI commands travel through the TX queue and DMA engine; controller replies return through circular RX DMA, UART IDLE, producer/consumer tasks, and the parser."];
    subtitle.textColor = NSColor.secondaryLabelColor;
    subtitle.maximumNumberOfLines = 3;
    subtitle.lineBreakMode = NSLineBreakByWordWrapping;
    subtitle.translatesAutoresizingMaskIntoConstraints = NO;

    self.startButton = [NSButton buttonWithTitle:@"Start round trips"
                                         target:self
                                         action:@selector(startDemo:)];
    self.startButton.bezelStyle = NSBezelStyleRounded;
    self.startButton.keyEquivalent = @"\r";
    self.startButton.translatesAutoresizingMaskIntoConstraints = NO;

    self.progress = [[NSProgressIndicator alloc] init];
    self.progress.style = NSProgressIndicatorStyleSpinning;
    self.progress.displayedWhenStopped = NO;
    self.progress.translatesAutoresizingMaskIntoConstraints = NO;

    self.statusLabel = [NSTextField labelWithString:@"Ready"];
    self.statusLabel.font = [NSFont systemFontOfSize:13.0 weight:NSFontWeightMedium];
    self.statusLabel.translatesAutoresizingMaskIntoConstraints = NO;

    NSScrollView *scrollView = [[NSScrollView alloc] init];
    scrollView.hasVerticalScroller = YES;
    scrollView.borderType = NSBezelBorder;
    scrollView.translatesAutoresizingMaskIntoConstraints = NO;

    self.logView = [[NSTextView alloc] init];
    self.logView.editable = NO;
    self.logView.selectable = YES;
    self.logView.richText = NO;
    self.logView.backgroundColor = NSColor.textBackgroundColor;
    self.logView.textContainerInset = NSMakeSize(10.0, 10.0);
    scrollView.documentView = self.logView;

    [content addSubview:title];
    [content addSubview:subtitle];
    [content addSubview:self.startButton];
    [content addSubview:self.progress];
    [content addSubview:self.statusLabel];
    [content addSubview:scrollView];

    [NSLayoutConstraint activateConstraints:@[
        [title.topAnchor constraintEqualToAnchor:content.topAnchor constant:24.0],
        [title.leadingAnchor constraintEqualToAnchor:content.leadingAnchor constant:24.0],
        [title.trailingAnchor constraintLessThanOrEqualToAnchor:content.trailingAnchor constant:-24.0],

        [subtitle.topAnchor constraintEqualToAnchor:title.bottomAnchor constant:8.0],
        [subtitle.leadingAnchor constraintEqualToAnchor:title.leadingAnchor],
        [subtitle.trailingAnchor constraintEqualToAnchor:content.trailingAnchor constant:-24.0],

        [self.startButton.topAnchor constraintEqualToAnchor:subtitle.bottomAnchor constant:18.0],
        [self.startButton.leadingAnchor constraintEqualToAnchor:title.leadingAnchor],
        [self.progress.centerYAnchor constraintEqualToAnchor:self.startButton.centerYAnchor],
        [self.progress.leadingAnchor constraintEqualToAnchor:self.startButton.trailingAnchor constant:14.0],
        [self.statusLabel.centerYAnchor constraintEqualToAnchor:self.startButton.centerYAnchor],
        [self.statusLabel.leadingAnchor constraintEqualToAnchor:self.progress.trailingAnchor constant:10.0],

        [scrollView.topAnchor constraintEqualToAnchor:self.startButton.bottomAnchor constant:18.0],
        [scrollView.leadingAnchor constraintEqualToAnchor:content.leadingAnchor constant:24.0],
        [scrollView.trailingAnchor constraintEqualToAnchor:content.trailingAnchor constant:-24.0],
        [scrollView.bottomAnchor constraintEqualToAnchor:content.bottomAnchor constant:-24.0]
    ]];

    [self.window center];
    [self.window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
}

/** Terminate the process when the only application window closes. */
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)sender {
    (void)sender;
    return YES;
}

/** Stop a still-running helper during application shutdown. */
- (void)applicationWillTerminate:(NSNotification *)notification {
    (void)notification;
    if (self.demoTask.running) {
        [self.demoTask terminate];
    }
}

@end

/** Install the minimal application menu and standard Quit command. */
static void installApplicationMenu(void) {
    NSMenu *menuBar = [[NSMenu alloc] init];
    NSMenuItem *applicationItem = [[NSMenuItem alloc] init];
    [menuBar addItem:applicationItem];

    NSMenu *applicationMenu = [[NSMenu alloc] init];
    NSString *quitTitle = [@"Quit " stringByAppendingString:NSProcessInfo.processInfo.processName];
    NSMenuItem *quitItem = [[NSMenuItem alloc] initWithTitle:quitTitle
                                                    action:@selector(terminate:)
                                             keyEquivalent:@"q"];
    [applicationMenu addItem:quitItem];
    applicationItem.submenu = applicationMenu;
    NSApp.mainMenu = menuBar;
}

/** Configure and enter the Cocoa application event loop. */
int main(void) {
    @autoreleasepool {
        NSApplication *application = NSApplication.sharedApplication;
        UartDemoAppDelegate *delegate = [[UartDemoAppDelegate alloc] init];
        application.delegate = delegate;
        [application setActivationPolicy:NSApplicationActivationPolicyRegular];
        installApplicationMenu();
        [application run];
    }
    return 0;
}
