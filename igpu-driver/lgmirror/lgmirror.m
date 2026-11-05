// lgmirror - feeds the iGPU-lit LG monitor (IGPUScanout) with a macOS virtual display.
//
// 1. opens IGPUScanout and asks it to point plane 1 of pipe A at a buffer it owns,
// 2. creates a virtual display of the same size (WindowServer draws it on the RTX 5090 like any screen),
// 3. captures that display with ScreenCaptureKit and copies each frame into the mapped buffer.
// Ctrl-C (or killing it) gives the plane back to the firmware: the LG goes black again.
//
// Usage:  lgmirror            firmware geometry (1920x1080, upscaled to 4K by the iGPU)
//         lgmirror --4k       native 3840x2160 plane, virtual display "looks like 1920x1080" HiDPI
//         lgmirror --info     print the iGPU state and exit
// Needs Screen Recording permission for the app that runs it (Terminal), asked on first run.
#import <Foundation/Foundation.h>
#import <CoreGraphics/CoreGraphics.h>
#import <IOKit/IOKitLib.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#include <signal.h>
#include <string.h>

// ---- CGVirtualDisplay (private CoreGraphics API, used the same way by DeskPad/BetterDisplay) ----
@interface CGVirtualDisplayDescriptor : NSObject
@property(retain, nonatomic) dispatch_queue_t queue;
@property(retain, nonatomic) NSString *name;
@property(nonatomic) unsigned int maxPixelsWide, maxPixelsHigh;
@property(nonatomic) CGSize sizeInMillimeters;
@property(nonatomic) unsigned int productID, vendorID, serialNum;
@property(copy, nonatomic) void (^terminationHandler)(id, id);
@end
@interface CGVirtualDisplayMode : NSObject
- (instancetype)initWithWidth:(NSUInteger)width height:(NSUInteger)height refreshRate:(double)refreshRate;
@end
@interface CGVirtualDisplaySettings : NSObject
@property(retain, nonatomic) NSArray *modes;
@property(nonatomic) unsigned int hiDPI;
@end
@interface CGVirtualDisplay : NSObject
- (instancetype)initWithDescriptor:(CGVirtualDisplayDescriptor *)descriptor;
- (BOOL)applySettings:(CGVirtualDisplaySettings *)settings;
@property(readonly, nonatomic) CGDirectDisplayID displayID;
@end

static io_connect_t gConn = IO_OBJECT_NULL;
static volatile sig_atomic_t gStop = 0;

static io_connect_t openScanout(void) {
    io_service_t s = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("IGPUScanout"));
    if (!s) { fprintf(stderr, "IGPUScanout is not loaded (boot with -igpuscanout)\n"); return IO_OBJECT_NULL; }
    io_connect_t c = IO_OBJECT_NULL;
    kern_return_t kr = IOServiceOpen(s, mach_task_self(), 0, &c);
    IOObjectRelease(s);
    if (kr != KERN_SUCCESS) { fprintf(stderr, "IOServiceOpen failed: 0x%x\n", kr); return IO_OBJECT_NULL; }
    return c;
}

static void printInfo(io_connect_t c) {
    uint64_t o[16] = {0}; uint32_t n = 16;
    kern_return_t kr = IOConnectCallScalarMethod(c, 0, NULL, 0, o, &n);
    if (kr) { fprintf(stderr, "getInfo failed 0x%x\n", kr); return; }
    printf("TRANSCONF_A %08llx  PIPESRC %08llx  PLANE_CTL %08llx STRIDE %08llx SIZE %08llx SURF %08llx\n",
           o[0], o[1], o[2], o[3], o[4], o[5]);
    printf("PS1_CTRL %08llx  BUF_CFG %08llx  WM0 %08llx  WM_TRANS %08llx  FRMCOUNT %llu\n", o[6], o[7], o[8], o[9], o[10]);
    printf("firmware PTE %016llx  our-range PTE %016llx  attached %llu mode %llu  GMD_ID %08llx\n",
           o[11], o[12], o[13], o[14], o[15]);
}

static void onSignal(int sig) { (void)sig; gStop = 1; }

@interface Mirror : NSObject <SCStreamOutput, SCStreamDelegate>
@property(nonatomic) uint8_t *dst;
@property(nonatomic) uint32_t width, height, stride;
@property(nonatomic) uint64_t frames;
@property(strong, nonatomic) SCStream *stream;
@end

@implementation Mirror
- (void)stream:(SCStream *)stream didOutputSampleBuffer:(CMSampleBufferRef)sb ofType:(SCStreamOutputType)type {
    if (type != SCStreamOutputTypeScreen || !CMSampleBufferIsValid(sb)) return;
    CFArrayRef att = CMSampleBufferGetSampleAttachmentsArray(sb, false);
    if (att && CFArrayGetCount(att) > 0) {
        NSDictionary *info = (__bridge NSDictionary *)CFArrayGetValueAtIndex(att, 0);
        NSNumber *st = info[SCStreamFrameInfoStatus];
        if (st && st.integerValue != SCFrameStatusComplete) return;
    }
    CVPixelBufferRef pb = CMSampleBufferGetImageBuffer(sb);
    if (!pb) return;
    CVPixelBufferLockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
    const uint8_t *src = CVPixelBufferGetBaseAddress(pb);
    const size_t srcStride = CVPixelBufferGetBytesPerRow(pb);
    const size_t w = MIN(CVPixelBufferGetWidth(pb), self.width), h = MIN(CVPixelBufferGetHeight(pb), self.height);
    if (src) for (size_t y = 0; y < h; y++) memcpy(self.dst + y * self.stride, src + y * srcStride, w * 4);
    CVPixelBufferUnlockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
    self.frames++;
}
- (void)stream:(SCStream *)stream didStopWithError:(NSError *)error {
    fprintf(stderr, "capture stopped: %s\n", error.localizedDescription.UTF8String);
    gStop = 1;
}
@end

int main(int argc, const char *argv[]) {
    @autoreleasepool {
        BOOL want4k = NO, infoOnly = NO, testOnly = NO;
        for (int i = 1; i < argc; i++) {
            if (!strcmp(argv[i], "--4k")) want4k = YES;
            else if (!strcmp(argv[i], "--info")) infoOnly = YES;
            else if (!strcmp(argv[i], "--test")) testOnly = YES;
            else { fprintf(stderr, "usage: lgmirror [--4k] [--info] [--test]\n"); return 2; }
        }
        gConn = openScanout();
        if (!gConn) return 1;
        printInfo(gConn);
        if (infoOnly) { IOServiceClose(gConn); return 0; }

        uint64_t in = want4k ? 1 : 0, o[4] = {0}; uint32_t n = 4;
        kern_return_t kr = IOConnectCallScalarMethod(gConn, 1, &in, 1, o, &n);
        if (kr) { fprintf(stderr, "attach refused: 0x%x (see: ioreg -lw0 -c IGPUScanout | grep ISOStatus)\n", kr); return 1; }
        const uint32_t W = (uint32_t)o[0], H = (uint32_t)o[1], S = (uint32_t)o[2];
        mach_vm_address_t addr = 0; mach_vm_size_t size = 0;
        kr = IOConnectMapMemory64(gConn, 0, mach_task_self(), &addr, &size, kIOMapAnywhere);
        if (kr) { fprintf(stderr, "map failed 0x%x\n", kr); IOConnectCallScalarMethod(gConn, 2, NULL, 0, NULL, NULL); return 1; }
        printf("plane 1 now scans our %ux%u buffer (stride %u, %llu bytes) at GGTT %08llx\n", W, H, S, size, o[3]);

        signal(SIGINT, onSignal); signal(SIGTERM, onSignal); signal(SIGHUP, onSignal);

        if (testOnly) {   // colour bars straight into the scanout buffer: no virtual display, no screen capture
            static const uint32_t bars[8] = { 0xFFFFFF, 0xFFFF00, 0x00FFFF, 0x00FF00, 0xFF00FF, 0xFF0000, 0x0000FF, 0x000000 };
            for (uint32_t y = 0; y < H; y++) {
                uint32_t *row = (uint32_t *)((uint8_t *)addr + (size_t)y * S);
                for (uint32_t x = 0; x < W; x++) row[x] = (y > H * 3 / 4) ? ((x / 16 + y / 16) & 1 ? 0xFFFFFF : 0) : bars[x * 8 / W];
            }
            printf("colour bars on the LG (white, yellow, cyan, green, magenta, red, blue, black; checkerboard at the bottom).\nCtrl-C to stop.\n");
            while (!gStop) sleep(1);
            IOConnectUnmapMemory64(gConn, 0, mach_task_self(), addr);
            IOConnectCallScalarMethod(gConn, 2, NULL, 0, NULL, NULL);
            IOServiceClose(gConn);
            printf("stopped; the LG is back on the firmware plane\n");
            return 0;
        }

        // Virtual display: 1:1 with the plane in firmware mode, HiDPI 1920x1080@2x on a 4K plane.
        CGVirtualDisplayDescriptor *d = [CGVirtualDisplayDescriptor new];
        d.queue = dispatch_get_main_queue();
        d.name = @"LG (iGPU)";
        d.maxPixelsWide = W; d.maxPixelsHigh = H;
        d.sizeInMillimeters = CGSizeMake(597, 336);        // 27" 16:9
        d.vendorID = 0x1E6D; d.productID = 0x5B09; d.serialNum = 0xA2250001;
        d.terminationHandler = ^(id a, id b) { fprintf(stderr, "virtual display terminated\n"); gStop = 1; };
        CGVirtualDisplay *vd = [[CGVirtualDisplay alloc] initWithDescriptor:d];
        CGVirtualDisplaySettings *set = [CGVirtualDisplaySettings new];
        set.hiDPI = want4k ? 1 : 0;
        set.modes = @[ want4k ? [[CGVirtualDisplayMode alloc] initWithWidth:W / 2 height:H / 2 refreshRate:60]
                              : [[CGVirtualDisplayMode alloc] initWithWidth:W height:H refreshRate:60] ];
        if (!vd || ![vd applySettings:set]) { fprintf(stderr, "could not create the virtual display\n"); gStop = 1; }
        const CGDirectDisplayID vid = vd ? vd.displayID : 0;
        printf("virtual display %u created\n", vid);

        Mirror *m = [Mirror new];
        m.dst = (uint8_t *)addr; m.width = W; m.height = H; m.stride = S;

        // ScreenCaptureKit needs a moment before the new display shows up in its content list.
        __block BOOL started = NO;
        for (int tries = 0; !gStop && !started && tries < 20; tries++) {
            dispatch_semaphore_t sem = dispatch_semaphore_create(0);
            [SCShareableContent getShareableContentWithCompletionHandler:^(SCShareableContent *c, NSError *e) {
                if (e) {
                    fprintf(stderr, "screen capture refused: %s\nAllow Screen Recording for Terminal in the window that opens, quit Terminal (Cmd-Q), reopen it, run lgmirror again.\n", e.localizedDescription.UTF8String);
                    system("open \"x-apple.systempreferences:com.apple.preference.security?Privacy_ScreenCapture\"");
                    gStop = 1;
                }
                for (SCDisplay *disp in c.displays) {
                    if (disp.displayID != vid) continue;
                    SCContentFilter *f = [[SCContentFilter alloc] initWithDisplay:disp excludingWindows:@[]];
                    SCStreamConfiguration *cfg = [SCStreamConfiguration new];
                    cfg.width = W; cfg.height = H;
                    cfg.pixelFormat = kCVPixelFormatType_32BGRA;
                    cfg.minimumFrameInterval = CMTimeMake(1, 60);
                    cfg.showsCursor = YES;
                    cfg.queueDepth = 3;
                    m.stream = [[SCStream alloc] initWithFilter:f configuration:cfg delegate:m];
                    NSError *err = nil;
                    dispatch_queue_t q = dispatch_queue_create("lgmirror.frames", DISPATCH_QUEUE_SERIAL);
                    if (![m.stream addStreamOutput:m type:SCStreamOutputTypeScreen sampleHandlerQueue:q error:&err]) {
                        fprintf(stderr, "addStreamOutput: %s\n", err.localizedDescription.UTF8String); gStop = 1; break;
                    }
                    [m.stream startCaptureWithCompletionHandler:^(NSError *se) {
                        if (se) { fprintf(stderr, "startCapture: %s\n", se.localizedDescription.UTF8String); gStop = 1; }
                    }];
                    started = YES;
                    break;
                }
                dispatch_semaphore_signal(sem);
            }];
            while (dispatch_semaphore_wait(sem, DISPATCH_TIME_NOW))
                [[NSRunLoop mainRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.05]];
            if (!started && !gStop) [[NSRunLoop mainRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.5]];
        }
        if (!started && !gStop) { fprintf(stderr, "the virtual display never appeared to ScreenCaptureKit\n"); gStop = 1; }
        if (started) printf("mirroring; drag windows onto \"LG (iGPU)\". Ctrl-C to stop.\n");

        uint64_t last = 0;
        while (!gStop) {
            [[NSRunLoop mainRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:5]];
            if (started) { printf("frames: %llu (+%llu)\n", m.frames, m.frames - last); last = m.frames; fflush(stdout); }
        }

        if (m.stream) {
            dispatch_semaphore_t sem = dispatch_semaphore_create(0);
            [m.stream stopCaptureWithCompletionHandler:^(NSError *e) { dispatch_semaphore_signal(sem); }];
            dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC));
        }
        IOConnectUnmapMemory64(gConn, 0, mach_task_self(), addr);
        IOConnectCallScalarMethod(gConn, 2, NULL, 0, NULL, NULL);   // plane back to the firmware
        IOServiceClose(gConn);
        vd = nil;
        printf("stopped; the LG is back on the firmware plane\n");
    }
    return 0;
}
