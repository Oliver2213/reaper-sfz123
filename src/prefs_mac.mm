#import <Foundation/Foundation.h>

#include "prefs.h"

#include <cstdlib>

namespace sfp {

static NSString* const kSuite = @"dev.smoll.sfz123";
static const NSUInteger kMaxRecent = 12;

static NSUserDefaults* defaults()
{
    static NSUserDefaults* d = [[NSUserDefaults alloc] initWithSuiteName:kSuite];
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
    if (path.empty() || std::getenv("SFZ123_NO_RECENTS"))
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

bool nameFxAfterInstrument()
{
    id v = [defaults() objectForKey:@"nameFxAfterInstrument"];
    return v ? [v boolValue] : true;
}

void setNameFxAfterInstrument(bool on)
{
    [defaults() setBool:on forKey:@"nameFxAfterInstrument"];
}

} // namespace sfp
