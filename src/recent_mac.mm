#import <Foundation/Foundation.h>

#include "recent.h"

#include <cstdlib>

namespace sfp {

static NSString* const kSuite = @"com.oliver2213.sf-player";
static const NSUInteger kMaxRecent = 12;

static NSUserDefaults* defaults()
{
    static NSUserDefaults* d = [] {
        NSUserDefaults* d = [[NSUserDefaults alloc] initWithSuiteName:kSuite];
        NSUserDefaults* old = [[NSUserDefaults alloc] initWithSuiteName:@"com.sfplayer.sf-player"];
        for (NSString* key in @[ @"recentFiles", @"lastDirectory", @"selectedTab" ])
            if (![d objectForKey:key] && [old objectForKey:key])
                [d setObject:[old objectForKey:key] forKey:key];
        return d;
    }();
    return d;
}

std::vector<std::string> recentInstruments()
{
    std::vector<std::string> out;
    NSArray* a = [defaults() arrayForKey:@"recentFiles"];
    for (id p in a)
        if ([p isKindOfClass:[NSString class]])
            out.push_back([p fileSystemRepresentation]);
    return out;
}

void addRecentInstrument(const std::string& path)
{
    if (path.empty() || std::getenv("SFP_NO_RECENTS"))
        return;
    NSString* p = [NSString stringWithUTF8String:path.c_str()];
    if (!p)
        return;
    NSMutableArray* a = [[defaults() arrayForKey:@"recentFiles"] mutableCopy] ?: [NSMutableArray array];
    [a removeObject:p];
    [a insertObject:p atIndex:0];
    while (a.count > kMaxRecent)
        [a removeLastObject];
    [defaults() setObject:a forKey:@"recentFiles"];
}

} // namespace sfp
