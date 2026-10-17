// iOS replacement for runtime/cemu/Cafe/HW/Latte/Renderer/Metal/MetalLayer.mm + MetalView.mm (AppKit).
// `handle` is the UIView of SDL's window (see runtime/src/host/window.mm); the renderer gets a
// CAMetalLayer-backed child view filling it at the screen's native scale. The child ignores touches
// so they still reach SDL's view.
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <UIKit/UIKit.h>

#include "log.h"

@interface BotWMetalView : UIView
@end
@implementation BotWMetalView
+ (Class)layerClass {
    return [CAMetalLayer class];
}
@end

void* CreateMetalLayer(void* handle, float& scaleX, float& scaleY) {
    UIView* parent = (__bridge UIView*)handle;
    BotWMetalView* view = [[BotWMetalView alloc] initWithFrame:parent.bounds];
    view.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    view.userInteractionEnabled = NO;
    UIScreen* screen = parent.window.windowScene.screen ?: UIScreen.mainScreen;
    CGFloat scale = screen.nativeScale;
    view.contentScaleFactor = scale;
    view.layer.contentsScale = scale;
    [parent addSubview:view];
    scaleX = scaleY = (float)scale;
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
    LOG("ios: GPU %s, Apple9 %d, BC textures %d, unified memory %d, recommended working set %llu MB, "
        "view %.0fx%.0f pt @%.1fx",
        dev.name.UTF8String, [dev supportsFamily:MTLGPUFamilyApple9], dev.supportsBCTextureCompression,
        dev.hasUnifiedMemory, (unsigned long long)(dev.recommendedMaxWorkingSetSize >> 20),
        parent.bounds.size.width, parent.bounds.size.height, scale);
    // +1 like the AppKit version's layer handle expects (MetalLayerHandle releases it)
    return (__bridge_retained void*)view.layer;
}
