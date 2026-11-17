// Opens the plugin editor in a window, saves a PNG snapshot and prints the accessibility tree.
//   sfz123_gui_test "<sfz123.clap>" <instrument> <out.png> [tab-index]
#import <Cocoa/Cocoa.h>
#include <clap/clap.h>
#include <dlfcn.h>
#include <string>

static bool gCallback = false;
static const void* hostExt(const clap_host*, const char*) { return nullptr; }
static void noop(const clap_host*) {}
static void reqCallback(const clap_host*) { gCallback = true; }
static clap_host gHost = { CLAP_VERSION_INIT, nullptr, "gui_test", "", "", "1", hostExt, noop, noop, reqCallback };

struct Stream {
    std::string data;
    size_t pos = 0;
};
static int64_t sRead(const clap_istream* s, void* buf, uint64_t n)
{
    Stream* st = (Stream*)s->ctx;
    n = std::min<uint64_t>(n, st->data.size() - st->pos);
    memcpy(buf, st->data.data() + st->pos, n);
    st->pos += n;
    return (int64_t)n;
}

static void dumpAx(id element, int depth)
{
    if (depth > 14 || ![element respondsToSelector:@selector(accessibilityRole)])
        return;
    NSString* role = [element accessibilityRole];
    NSString* label = [element accessibilityLabel];
    id value = [element respondsToSelector:@selector(accessibilityValue)] ? [element accessibilityValue] : nil;
    NSString* desc = [element respondsToSelector:@selector(accessibilityValueDescription)] ? [element accessibilityValueDescription] : nil;
    NSString* title = [element respondsToSelector:@selector(accessibilityTitle)] ? [element accessibilityTitle] : nil;
    BOOL enabled = [element respondsToSelector:@selector(isAccessibilityEnabled)] ? [element isAccessibilityEnabled] : YES;
    if ([role isEqualToString:NSAccessibilityTabGroupRole])
        enabled = YES;
    if (role && ![role isEqualToString:NSAccessibilityGroupRole] && ![role isEqualToString:NSAccessibilityScrollAreaRole]
        && ![role isEqualToString:NSAccessibilityUnknownRole]) {
        NSMutableString* line = [NSMutableString stringWithFormat:@"%*s%@", depth * 2, "", role];
        if (label.length) [line appendFormat:@" label=\"%@\"", label];
        if (title.length) [line appendFormat:@" title=\"%@\"", title];
        if (desc.length) [line appendFormat:@" valueDesc=\"%@\"", desc];
        else if (value && [[value description] length] && [[value description] length] < 400) [line appendFormat:@" value=\"%@\"", value];
        if (!enabled) [line appendString:@" (dimmed)"];
        printf("%s\n", line.UTF8String);
    }
    if ([role isEqualToString:NSAccessibilityTabGroupRole])
        for (id tab in [element accessibilityTabs] ?: @[])
            printf("%*s  tab: %s\n", depth * 2, "", ([tab isKindOfClass:[NSTabViewItem class]] ? [(NSTabViewItem*)tab label] : [tab description]).UTF8String);
    NSString* help = [element respondsToSelector:@selector(accessibilityHelp)] ? [element accessibilityHelp] : nil;
    if (help.length && getenv("AX_HELP"))
        printf("%*s  help: %s\n", depth * 2, "", help.UTF8String);
    for (id child in [element accessibilityChildren] ?: @[])
        dumpAx(child, depth + 1);
}

int main(int argc, char** argv)
{
    setenv("SFZ123_NO_RECENTS", "1", 1); // keep test loads out of the user's recent instruments
    @autoreleasepool {
        if (argc < 4)
            return 2;
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];

        std::string bundle = argv[1];
        void* lib = dlopen((bundle + "/Contents/MacOS/sfz123").c_str(), RTLD_NOW);
        auto* entry = (const clap_plugin_entry*)dlsym(lib, "clap_entry");
        entry->init(bundle.c_str());
        auto* f = (const clap_plugin_factory*)entry->get_factory(CLAP_PLUGIN_FACTORY_ID);
        const clap_plugin* p = f->create_plugin(f, &gHost, f->get_plugin_descriptor(f, 0)->id);
        p->init(p);

        auto* state = (const clap_plugin_state*)p->get_extension(p, CLAP_EXT_STATE);
        Stream st;
        st.data = std::string("SFZ123 1\npath=") + argv[2] + "\n";
        clap_istream is { &st, sRead };
        state->load(p, &is);

        auto* gui = (const clap_plugin_gui*)p->get_extension(p, CLAP_EXT_GUI);
        gui->create(p, CLAP_WINDOW_API_COCOA, false);
        uint32_t w, h;
        gui->get_size(p, &w, &h);
        NSWindow* win = [[NSWindow alloc] initWithContentRect:NSMakeRect(100, 100, w, h)
                                                    styleMask:NSWindowStyleMaskTitled
                                                      backing:NSBackingStoreBuffered
                                                        defer:NO];
        clap_window cw { CLAP_WINDOW_API_COCOA, { (__bridge void*)win.contentView } };
        gui->set_parent(p, &cw);
        gui->show(p);
        [win orderFront:nil];

        NSDate* until = [NSDate dateWithTimeIntervalSinceNow:4.0];
        while ([until timeIntervalSinceNow] > 0) {
            if (gCallback) {
                gCallback = false;
                p->on_main_thread(p);
            }
            [[NSRunLoop mainRunLoop] runMode:NSDefaultRunLoopMode beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.05]];
        }

        NSView* root = win.contentView.subviews.firstObject;
        if (argc > 4) {
            NSTabView* tabs = nil;
            NSMutableArray* q = [NSMutableArray arrayWithObject:root];
            while (q.count && !tabs) {
                NSView* v = q.firstObject;
                [q removeObjectAtIndex:0];
                if ([v isKindOfClass:[NSTabView class]])
                    tabs = (NSTabView*)v;
                [q addObjectsFromArray:v.subviews];
            }
            [tabs selectTabViewItemAtIndex:atoi(argv[4])];
            [[NSRunLoop mainRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.3]];
        }

        [root layoutSubtreeIfNeeded];
        NSBitmapImageRep* rep = [root bitmapImageRepForCachingDisplayInRect:root.bounds];
        [root cacheDisplayInRect:root.bounds toBitmapImageRep:rep];
        [[rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}] writeToFile:@(argv[3]) atomically:YES];
        printf("wrote %s (%ux%u)\n", argv[3], w, h);

        dumpAx(root, 0);

        gui->destroy(p);
        p->destroy(p);
        entry->deinit();
    }
    return 0;
}
