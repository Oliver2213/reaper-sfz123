// Native AppKit editor. Everything is a standard Cocoa control so VoiceOver, Full Keyboard
// Access and system text scaling work as they do in any Mac app.
#import <Cocoa/Cocoa.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include "plugin.h"

#include <cmath>

using namespace sfp;

static const CGFloat kWidth = 740;
static const CGFloat kHeight = 660;
static NSString* const kDefaultsSuite = @"com.oliver2213.sf-player";
static const NSInteger kMaxRecent = 12;

static NSString* ns(const std::string& s)
{
    return [NSString stringWithUTF8String:s.c_str()] ?: @"";
}

static void announce(NSString* text)
{
    if (text.length == 0)
        return;
    NSAccessibilityPostNotificationWithUserInfo(NSApp, NSAccessibilityAnnouncementRequestedNotification, @{
        NSAccessibilityAnnouncementKey : text,
        NSAccessibilityPriorityKey : @(NSAccessibilityPriorityHigh),
    });
}

static NSArray<NSString*>* instrumentExtensions()
{
    return @[ @"sf2", @"sf3", @"sfz" ];
}

static BOOL isInstrumentURL(NSURL* url)
{
    return [instrumentExtensions() containsObject:url.pathExtension.lowercaseString];
}

#pragma mark - Flipped document view

@interface SFPFlippedView : NSView
@end
@implementation SFPFlippedView
- (BOOL)isFlipped
{
    return YES;
}
@end

#pragma mark - Slider

enum GesturePhase { kBegin, kChange, kEnd, kSingle };

@interface SFPSlider : NSSlider
@property(nonatomic, copy) NSString* (^describe)(double value);
@property(nonatomic, copy) void (^onUserChange)(double value, GesturePhase phase);
@property(nonatomic) double step;
@property(nonatomic) BOOL dragging;
@end

// AppKit exposes cell-based controls to VoiceOver through their cell, so the spoken value
// and the increment/decrement actions live here.
@interface SFPSliderCell : NSSliderCell
@end

@implementation SFPSlider

+ (Class)cellClass
{
    return [SFPSliderCell class];
}

- (NSString*)accessibilityValueDescription
{
    return self.describe ? self.describe(self.doubleValue) : [super accessibilityValueDescription];
}

- (void)nudge:(double)steps
{
    double v = std::clamp(self.doubleValue + steps * self.step, self.minValue, self.maxValue);
    if (v == self.doubleValue)
        return;
    self.doubleValue = v;
    if (self.onUserChange)
        self.onUserChange(v, kSingle);
    NSAccessibilityPostNotification(self, NSAccessibilityValueChangedNotification);
}

- (BOOL)accessibilityPerformIncrement
{
    [self nudge:1];
    return YES;
}

- (BOOL)accessibilityPerformDecrement
{
    [self nudge:-1];
    return YES;
}

- (void)setToValue:(double)v
{
    self.doubleValue = v;
    if (self.onUserChange)
        self.onUserChange(v, kSingle);
    NSAccessibilityPostNotification(self, NSAccessibilityValueChangedNotification);
}

- (void)keyDown:(NSEvent*)e
{
    NSString* c = e.charactersIgnoringModifiers;
    unichar k = c.length ? [c characterAtIndex:0] : 0;
    double mult = (e.modifierFlags & NSEventModifierFlagShift) ? 10 : 1;
    switch (k) {
    case NSUpArrowFunctionKey:
    case NSRightArrowFunctionKey: [self nudge:mult]; return;
    case NSDownArrowFunctionKey:
    case NSLeftArrowFunctionKey: [self nudge:-mult]; return;
    case NSPageUpFunctionKey: [self nudge:10]; return;
    case NSPageDownFunctionKey: [self nudge:-10]; return;
    case NSHomeFunctionKey: [self setToValue:self.minValue]; return;
    case NSEndFunctionKey: [self setToValue:self.maxValue]; return;
    default: [super keyDown:e];
    }
}

@end

@implementation SFPSliderCell

- (NSString*)accessibilityValueDescription
{
    SFPSlider* s = (SFPSlider*)self.controlView;
    return [s isKindOfClass:[SFPSlider class]] ? [s accessibilityValueDescription] : [super accessibilityValueDescription];
}

- (BOOL)accessibilityPerformIncrement
{
    SFPSlider* s = (SFPSlider*)self.controlView;
    return [s isKindOfClass:[SFPSlider class]] ? [s accessibilityPerformIncrement] : [super accessibilityPerformIncrement];
}

- (BOOL)accessibilityPerformDecrement
{
    SFPSlider* s = (SFPSlider*)self.controlView;
    return [s isKindOfClass:[SFPSlider class]] ? [s accessibilityPerformDecrement] : [super accessibilityPerformDecrement];
}

@end

// Labels both the control and its cell (VoiceOver may talk to either).
static void setAx(NSControl* c, NSString* label, NSString* help)
{
    c.accessibilityLabel = label;
    c.cell.accessibilityLabel = label;
    if (help) {
        c.accessibilityHelp = help;
        c.cell.accessibilityHelp = help;
        c.toolTip = help;
    }
}

#pragma mark - Root view

@class SFPRootView;

@interface SFPParamRow : NSObject
@property(nonatomic) clap_id paramId;
@property(nonatomic, strong) NSTextField* label;
@property(nonatomic, strong) NSControl* control;
@property(nonatomic, strong) NSTextField* valueField;
@property(nonatomic) double shown;
@end

@implementation SFPParamRow
@end

@interface SFPRootView : NSView <NSTextFieldDelegate, NSTabViewDelegate>
- (instancetype)initWithPlugin:(Plugin*)plugin;
- (void)shutdown;
@end

@implementation SFPRootView {
    Plugin* _plugin;
    NSTimer* _timer;
    uint64_t _revision;
    NSString* _lastAnnounced;
    NSString* _lastRecentPath;
    int _lastPercent;
    int _tick;

    NSTextField* _instrumentField;
    NSTextField* _pathField;
    NSTextField* _locationField;
    NSTextField* _statusField;
    NSTextField* _voicesField;
    NSPopUpButton* _recentButton;
    NSButton* _reloadButton;
    NSButton* _finderButton;
    NSButton* _unloadButton;
    NSButton* _prevFileButton;
    NSButton* _nextFileButton;

    NSStackView* _presetRow;
    NSPopUpButton* _presetPopup;
    NSButton* _prevPresetButton;
    NSButton* _nextPresetButton;
    NSInteger _presetCount;

    NSTabView* _tabs;
    NSMutableArray<SFPParamRow*>* _rows;
    NSMutableDictionary<NSNumber*, NSGridView*>* _grids;
    NSMutableArray<NSTextField*>* _sf2Notices;
    NSTextField* _noControlsNote;
}

- (BOOL)isFlipped
{
    return YES;
}

- (instancetype)initWithPlugin:(Plugin*)plugin
{
    self = [super initWithFrame:NSMakeRect(0, 0, kWidth, kHeight)];
    if (!self)
        return nil;
    _plugin = plugin;
    _rows = [NSMutableArray array];
    _grids = [NSMutableDictionary dictionary];
    _sf2Notices = [NSMutableArray array];
    _lastPercent = -1;

    self.wantsLayer = YES;
    self.accessibilityLabel = @"SF Player";
    self.accessibilityRole = NSAccessibilityGroupRole;

    [self buildInterface];
    [self registerForDraggedTypes:@[ NSPasteboardTypeFileURL ]];
    [self refreshInstrument];
    [self refreshValues:YES];

    __weak SFPRootView* weakSelf = self;
    _timer = [NSTimer timerWithTimeInterval:0.1
                                    repeats:YES
                                      block:^(NSTimer*) {
                                          [weakSelf tick];
                                      }];
    [[NSRunLoop mainRunLoop] addTimer:_timer forMode:NSRunLoopCommonModes];
    return self;
}

- (void)shutdown
{
    [_timer invalidate];
    _timer = nil;
    _plugin = nullptr;
}

- (void)drawRect:(NSRect)r
{
    [[NSColor windowBackgroundColor] setFill];
    NSRectFill(r);
}

#pragma mark - Widgets

static NSTextField* makeLabel(NSString* text)
{
    NSTextField* f = [NSTextField labelWithString:text];
    f.lineBreakMode = NSLineBreakByTruncatingTail;
    return f;
}

static NSTextField* makeWrappingText(NSString* label)
{
    NSTextField* f = [NSTextField wrappingLabelWithString:@""];
    f.selectable = YES;
    f.accessibilityLabel = label;
    f.preferredMaxLayoutWidth = kWidth - 40;
    return f;
}

- (NSButton*)button:(NSString*)title action:(SEL)action help:(NSString*)help
{
    NSButton* b = [NSButton buttonWithTitle:title target:self action:action];
    setAx(b, title, help);
    return b;
}

- (void)buildInterface
{
    NSStackView* root = [[NSStackView alloc] init];
    root.orientation = NSUserInterfaceLayoutOrientationVertical;
    root.alignment = NSLayoutAttributeLeading;
    root.spacing = 8;
    root.edgeInsets = NSEdgeInsetsMake(14, 18, 14, 18);
    root.translatesAutoresizingMaskIntoConstraints = NO;
    [self addSubview:root];
    [NSLayoutConstraint activateConstraints:@[
        [root.leadingAnchor constraintEqualToAnchor:self.leadingAnchor],
        [root.trailingAnchor constraintEqualToAnchor:self.trailingAnchor],
        [root.topAnchor constraintEqualToAnchor:self.topAnchor],
        [root.bottomAnchor constraintEqualToAnchor:self.bottomAnchor],
    ]];

    NSButton* open = [self button:@"Open…" action:@selector(openDocument:) help:@"Choose an SF2, SF3 or SFZ file to load. You can also drop a file on this window."];
    _recentButton = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:YES];
    setAx(_recentButton, @"Recent instruments", @"Load an instrument you used recently.");
    _prevFileButton = [self button:@"Previous File" action:@selector(previousFile:) help:@"Load the previous instrument file in the same folder."];
    _nextFileButton = [self button:@"Next File" action:@selector(nextFile:) help:@"Load the next instrument file in the same folder."];
    _reloadButton = [self button:@"Reload" action:@selector(reload:) help:@"Load the current file again, for example after editing an SFZ file."];
    _finderButton = [self button:@"Show in Finder" action:@selector(showInFinder:) help:@"Reveal the instrument file in Finder."];
    _unloadButton = [self button:@"Unload" action:@selector(unload:) help:@"Remove the instrument and free its memory."];

    NSStackView* fileRow = [NSStackView stackViewWithViews:@[ open, _recentButton, _prevFileButton, _nextFileButton, _reloadButton, _finderButton, _unloadButton ]];
    fileRow.spacing = 6;
    [root addArrangedSubview:fileRow];
    [self rebuildRecentMenu];

    _instrumentField = makeWrappingText(@"Instrument");
    _instrumentField.accessibilityRole = NSAccessibilityStaticTextRole;
    _instrumentField.font = [NSFont boldSystemFontOfSize:[NSFont systemFontSize] + 2];
    [root addArrangedSubview:_instrumentField];

    _pathField = makeWrappingText(@"File path");
    _pathField.textColor = [NSColor secondaryLabelColor];
    _pathField.font = [NSFont systemFontOfSize:[NSFont smallSystemFontSize]];
    [root addArrangedSubview:_pathField];

    _locationField = makeWrappingText(@"Project location");
    _locationField.textColor = [NSColor secondaryLabelColor];
    _locationField.font = [NSFont systemFontOfSize:[NSFont smallSystemFontSize]];
    [root addArrangedSubview:_locationField];

    _statusField = makeWrappingText(@"Status");
    [root addArrangedSubview:_statusField];

    // presets: SF2/SF3 only
    NSTextField* presetLabel = makeLabel(@"Preset:");
    presetLabel.accessibilityElement = NO; // the popup carries the label
    presetLabel.cell.accessibilityElement = NO;
    _presetPopup = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
    _presetPopup.target = self;
    _presetPopup.action = @selector(presetChosen:);
    setAx(_presetPopup, @"Preset", @"The SoundFont preset to play: bank and program numbers, the name, and the keys that make a sound. Presets that cannot make any sound are left out.");
    [_presetPopup.widthAnchor constraintGreaterThanOrEqualToConstant:360].active = YES;
    _prevPresetButton = [self button:@"Previous Preset" action:@selector(previousPreset:) help:@"Select the previous preset."];
    _nextPresetButton = [self button:@"Next Preset" action:@selector(nextPreset:) help:@"Select the next preset."];
    _presetRow = [NSStackView stackViewWithViews:@[ presetLabel, _presetPopup, _prevPresetButton, _nextPresetButton ]];
    _presetRow.spacing = 6;
    [root addArrangedSubview:_presetRow];

    _voicesField = makeLabel(@"Active voices: 0");
    _voicesField.textColor = [NSColor secondaryLabelColor];
    [root addArrangedSubview:_voicesField];

    // parameter tabs
    _tabs = [[NSTabView alloc] init];
    _tabs.delegate = self;
    _tabs.translatesAutoresizingMaskIntoConstraints = NO;
    [root addArrangedSubview:_tabs];
    [_tabs.widthAnchor constraintEqualToConstant:kWidth - 36].active = YES;
    [_tabs setContentHuggingPriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationVertical];
    [_tabs setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationVertical];

    for (Group g : { Group::Controls, Group::Main, Group::Sf2Playback, Group::Sf2Shaping, Group::Sf2Effects })
        [self addTabForGroup:g];

    NSString* savedTab = [[[NSUserDefaults alloc] initWithSuiteName:kDefaultsSuite] stringForKey:@"selectedTab"];
    if (savedTab) {
        NSInteger i = [_tabs indexOfTabViewItemWithIdentifier:savedTab];
        if (i != NSNotFound)
            [_tabs selectTabViewItemAtIndex:i];
    }
}

- (void)addTabForGroup:(Group)group
{
    NSMutableArray<NSArray<NSView*>*>* gridRows = [NSMutableArray array];

    for (const ParamDef& d : paramDefs()) {
        if (d.group != group || d.id == kPreset)
            continue;

        SFPParamRow* row = [[SFPParamRow alloc] init];
        row.paramId = d.id;
        row.shown = NAN;
        NSString* name = ns(d.name);
        NSString* help = ns(d.help);
        clap_id pid = d.id;
        __weak SFPRootView* weakSelf = self;

        row.label = makeLabel([name stringByAppendingString:@":"]);
        row.label.alignment = NSTextAlignmentRight;

        if (d.kind == Kind::Enum) {
            NSPopUpButton* p = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
            for (const char* c : d.choices)
                [p addItemWithTitle:ns(c)];
            p.target = self;
            p.action = @selector(popupChanged:);
            p.tag = (NSInteger)pid;
            row.control = p;
        } else if (d.kind == Kind::Toggle) {
            NSButton* b = [NSButton checkboxWithTitle:name target:self action:@selector(toggleChanged:)];
            b.tag = (NSInteger)pid;
            row.control = b;
            row.label = makeLabel(@"");
        } else {
            SFPSlider* s = [SFPSlider sliderWithValue:d.def minValue:d.min maxValue:d.max target:nil action:nil];
            s.continuous = YES;
            s.step = d.step;
            s.target = self;
            s.action = @selector(sliderMoved:);
            s.tag = (NSInteger)pid;
            s.describe = ^NSString*(double v) {
                SFPRootView* me = weakSelf;
                return me ? [me describeParam:pid value:v] : @"";
            };
            s.onUserChange = ^(double v, GesturePhase phase) {
                [weakSelf userChanged:pid value:v phase:phase];
            };
            [s.widthAnchor constraintEqualToConstant:300].active = YES;
            row.control = s;

            NSTextField* f = [NSTextField textFieldWithString:@""];
            f.delegate = self;
            f.tag = (NSInteger)pid;
            setAx(f, [name stringByAppendingString:@" value"], @"Type a new value and press Return.");
            [f.widthAnchor constraintEqualToConstant:170].active = YES;
            row.valueField = f;
        }

        setAx(row.control, name, help);
        row.label.accessibilityElement = NO; // the control carries the label
        row.label.cell.accessibilityElement = NO;

        [_rows addObject:row];
        [gridRows addObject:@[ row.label, row.control, row.valueField ?: [NSGridCell emptyContentView] ]];
    }

    NSView* content = [[NSView alloc] init];
    NSStackView* stack = [[NSStackView alloc] init];
    stack.orientation = NSUserInterfaceLayoutOrientationVertical;
    stack.alignment = NSLayoutAttributeLeading;
    stack.spacing = 10;
    stack.edgeInsets = NSEdgeInsetsMake(10, 10, 10, 10);
    stack.translatesAutoresizingMaskIntoConstraints = NO;

    if (group == Group::Sf2Playback || group == Group::Sf2Shaping || group == Group::Sf2Effects) {
        NSTextField* note = [NSTextField wrappingLabelWithString:@"These settings apply to SF2 and SF3 SoundFonts only. The loaded SFZ instrument does not use them."];
        note.textColor = [NSColor secondaryLabelColor];
        note.hidden = YES;
        [_sf2Notices addObject:note];
        [stack addArrangedSubview:note];
    }
    if (group == Group::Controls) {
        _noControlsNote = [NSTextField wrappingLabelWithString:@"Load an instrument to see its controls. SFZ instruments list the controls they define; SoundFonts show modulation, expression, sustain, reverb and chorus."];
        _noControlsNote.textColor = [NSColor secondaryLabelColor];
        [stack addArrangedSubview:_noControlsNote];
    }

    NSGridView* grid = [NSGridView gridViewWithViews:gridRows];
    grid.rowSpacing = 8;
    grid.columnSpacing = 10;
    grid.rowAlignment = NSGridRowAlignmentFirstBaseline;
    if (grid.numberOfColumns > 0) {
        [grid columnAtIndex:0].xPlacement = NSGridCellPlacementTrailing;
        [grid columnAtIndex:0].width = 190;
    }
    _grids[@((int)group)] = grid;
    [stack addArrangedSubview:grid];

    NSScrollView* scroll = [[NSScrollView alloc] init];
    scroll.hasVerticalScroller = YES;
    scroll.drawsBackground = NO;
    scroll.borderType = NSNoBorder;
    scroll.translatesAutoresizingMaskIntoConstraints = NO;

    NSView* doc = [[SFPFlippedView alloc] init];
    doc.translatesAutoresizingMaskIntoConstraints = NO;
    [doc addSubview:stack];
    scroll.documentView = doc;
    [NSLayoutConstraint activateConstraints:@[
        [stack.leadingAnchor constraintEqualToAnchor:doc.leadingAnchor],
        [stack.trailingAnchor constraintEqualToAnchor:doc.trailingAnchor],
        [stack.topAnchor constraintEqualToAnchor:doc.topAnchor],
        [stack.bottomAnchor constraintEqualToAnchor:doc.bottomAnchor],
        [doc.leadingAnchor constraintEqualToAnchor:scroll.contentView.leadingAnchor],
        [doc.trailingAnchor constraintEqualToAnchor:scroll.contentView.trailingAnchor],
        [doc.topAnchor constraintEqualToAnchor:scroll.contentView.topAnchor],
    ]];

    [content addSubview:scroll];
    [NSLayoutConstraint activateConstraints:@[
        [scroll.leadingAnchor constraintEqualToAnchor:content.leadingAnchor],
        [scroll.trailingAnchor constraintEqualToAnchor:content.trailingAnchor],
        [scroll.topAnchor constraintEqualToAnchor:content.topAnchor],
        [scroll.bottomAnchor constraintEqualToAnchor:content.bottomAnchor],
    ]];

    NSString* title = ns(groupTitle(group));
    NSTabViewItem* item = [[NSTabViewItem alloc] initWithIdentifier:title];
    item.label = title;
    item.view = content;
    [_tabs addTabViewItem:item];
}

- (void)tabView:(NSTabView*)tabView didSelectTabViewItem:(NSTabViewItem*)item
{
    [[[NSUserDefaults alloc] initWithSuiteName:kDefaultsSuite] setObject:item.identifier forKey:@"selectedTab"];
}

#pragma mark - Param plumbing

- (NSString*)describeParam:(clap_id)pid value:(double)v
{
    if (!_plugin)
        return @"";
    const ParamDef* d = findParam(pid);
    double q = (d && d->kind != Kind::Continuous) ? std::round(v) : v;
    return ns(_plugin->formatValue(pid, q));
}

- (void)userChanged:(clap_id)pid value:(double)v phase:(GesturePhase)phase
{
    if (!_plugin)
        return;
    if (phase == kBegin || phase == kSingle)
        _plugin->uiBeginGesture(pid);
    _plugin->uiSetParam(pid, v);
    if (phase == kEnd || phase == kSingle)
        _plugin->uiEndGesture(pid);
    [self refreshValues:NO];
}

- (void)sliderMoved:(SFPSlider*)s
{
    NSEvent* e = NSApp.currentEvent;
    clap_id pid = (clap_id)s.tag;
    if (e.type == NSEventTypeLeftMouseDown || e.type == NSEventTypeLeftMouseDragged) {
        if (!s.dragging) {
            s.dragging = YES;
            [self userChanged:pid value:s.doubleValue phase:kBegin];
        } else {
            [self userChanged:pid value:s.doubleValue phase:kChange];
        }
    } else if (e.type == NSEventTypeLeftMouseUp && s.dragging) {
        s.dragging = NO;
        [self userChanged:pid value:s.doubleValue phase:kEnd];
    } else {
        [self userChanged:pid value:s.doubleValue phase:kSingle];
    }
}

- (void)popupChanged:(NSPopUpButton*)p
{
    [self userChanged:(clap_id)p.tag value:(double)p.indexOfSelectedItem phase:kSingle];
}

- (void)toggleChanged:(NSButton*)b
{
    [self userChanged:(clap_id)b.tag value:b.state == NSControlStateValueOn ? 1.0 : 0.0 phase:kSingle];
}

- (void)controlTextDidEndEditing:(NSNotification*)n
{
    NSTextField* f = n.object;
    if (!_plugin || ![f isKindOfClass:[NSTextField class]])
        return;
    clap_id pid = (clap_id)f.tag;
    double v = 0;
    if (_plugin->paramsTextToValue(pid, f.stringValue.UTF8String, &v)) {
        [self userChanged:pid value:v phase:kSingle];
        announce([self describeParam:pid value:_plugin->param(pid)]);
    } else {
        NSBeep();
        announce(@"Not a valid value");
    }
    for (SFPParamRow* r in _rows)
        if (r.paramId == pid)
            r.shown = NAN; // force the field to show the canonical text
    [self refreshValues:NO];
}

- (BOOL)isEditing:(NSTextField*)f
{
    NSResponder* fr = self.window.firstResponder;
    return f.currentEditor != nil && (fr == f.currentEditor || fr == f);
}

- (void)refreshValues:(BOOL)force
{
    if (!_plugin)
        return;
    for (SFPParamRow* r in _rows) {
        double v = _plugin->param(r.paramId);
        if (!force && v == r.shown)
            continue;
        if ([r.control isKindOfClass:[SFPSlider class]]) {
            SFPSlider* s = (SFPSlider*)r.control;
            if (s.dragging && !force)
                continue;
            s.doubleValue = v;
        } else if ([r.control isKindOfClass:[NSPopUpButton class]]) {
            [(NSPopUpButton*)r.control selectItemAtIndex:(NSInteger)std::lround(v)];
        } else if ([r.control isKindOfClass:[NSButton class]]) {
            ((NSButton*)r.control).state = v >= 0.5 ? NSControlStateValueOn : NSControlStateValueOff;
        }
        if (r.valueField && ![self isEditing:r.valueField])
            r.valueField.stringValue = [self describeParam:r.paramId value:v];
        r.shown = v;
    }

    if (_presetCount > 0) {
        NSInteger sel = (NSInteger)std::lround(_plugin->param(kPreset));
        if (sel != _presetPopup.indexOfSelectedItem && sel < _presetCount)
            [_presetPopup selectItemAtIndex:sel];
    }
}

#pragma mark - Instrument state

- (void)refreshInstrument
{
    if (!_plugin)
        return;
    const InstrumentInfo& info = _plugin->instrument();
    const std::string& path = _plugin->requestedPath();
    BOOL loaded = info.kind != InstrumentKind::None;
    BOOL sfz = info.kind == InstrumentKind::Sfz;

    _instrumentField.stringValue = loaded ? ns(info.title + " (" + kindName(info.kind) + ")") : @"No instrument loaded";
    _pathField.stringValue = path.empty() ? @"" : ns(path);
    _pathField.hidden = path.empty();
    [self updateLocation];
    [self updateStatus];

    _reloadButton.enabled = !path.empty();
    _finderButton.enabled = !path.empty();
    _unloadButton.enabled = !path.empty();
    _prevFileButton.enabled = !path.empty();
    _nextFileButton.enabled = !path.empty();

    [_presetPopup removeAllItems];
    _presetCount = (NSInteger)info.presets.size();
    for (const PresetInfo& p : info.presets) {
        NSString* t = [NSString stringWithFormat:@"%03u:%03u  %@  (%@)", p.bank, p.program, ns(p.name), ns(keyRangeText(p.keyLo, p.keyHi))];
        [_presetPopup addItemWithTitle:@"x"];
        _presetPopup.lastItem.title = t; // avoid de-duplication by title
    }
    _presetRow.hidden = _presetCount == 0;

    // controls: names come from the instrument
    NSGridView* controlsGrid = _grids[@((int)Group::Controls)];
    NSInteger ci = 0;
    for (SFPParamRow* r in _rows) {
        const ParamDef* d = findParam(r.paramId);
        BOOL relevant = _plugin->paramRelevant(r.paramId);
        if (d->group == Group::Controls) {
            NSString* name = ns(_plugin->paramDisplayName(r.paramId));
            r.label.stringValue = [name stringByAppendingString:@":"];
            setAx(r.control, name, nil);
            setAx(r.valueField, [name stringByAppendingString:@" value"], nil);
            [controlsGrid rowAtIndex:ci].hidden = !relevant;
            ++ci;
        } else {
            r.control.enabled = relevant || !loaded;
            r.valueField.enabled = relevant || !loaded;
        }
        r.shown = NAN;
    }
    _noControlsNote.hidden = !info.controls.empty();
    if (loaded && info.controls.empty())
        _noControlsNote.stringValue = @"This instrument does not define any controls.";

    for (NSTextField* n in _sf2Notices)
        n.hidden = !sfz;

    [self refreshValues:YES];

    if (_plugin->loadState() == LoadState::Ready && !path.empty() && ![_lastRecentPath isEqualToString:ns(path)]) {
        _lastRecentPath = ns(path);
        [self addRecent:_lastRecentPath];
    }
}

- (void)updateLocation
{
    NSString* text = ns(_plugin->projectLocationText());
    if (![text isEqualToString:_locationField.stringValue])
        _locationField.stringValue = text;
    _locationField.hidden = text.length == 0;
}

- (void)updateStatus
{
    NSString* status = ns(_plugin->statusMessage());
    if (_plugin->loadState() == LoadState::Loading) {
        int pct = _plugin->loadProgress() / 10;
        if (pct > 0)
            status = [status stringByAppendingFormat:@" %d%%", pct];
    }
    _statusField.stringValue = status;
}

- (void)tick
{
    if (!_plugin)
        return;

    uint64_t rev = _plugin->revision();
    if (rev != _revision) {
        _revision = rev;
        [self refreshInstrument];
        // announce state changes, not every revision
        NSString* msg = ns(_plugin->statusMessage());
        if (_plugin->loadState() != LoadState::Empty && ![msg isEqualToString:_lastAnnounced]) {
            _lastAnnounced = msg;
            announce(msg);
        }
    } else if (_plugin->loadState() == LoadState::Loading) {
        [self updateStatus];
    }

    [self refreshValues:NO];

    if (++_tick % 3 == 0)
        _voicesField.stringValue = [NSString stringWithFormat:@"Active voices: %d", _plugin->activeVoices()];
    if (_tick % 10 == 0)
        [self updateLocation]; // changes when the project is saved or moved
}

#pragma mark - Actions

- (void)loadPath:(NSString*)path
{
    if (!_plugin || path.length == 0)
        return;
    [[[NSUserDefaults alloc] initWithSuiteName:kDefaultsSuite] setObject:path.stringByDeletingLastPathComponent forKey:@"lastDirectory"];
    _plugin->loadFile(path.fileSystemRepresentation);
}

- (void)openDocument:(id)sender
{
    NSOpenPanel* panel = [NSOpenPanel openPanel];
    panel.title = @"Open Instrument";
    panel.message = @"Choose an SF2, SF3 or SFZ instrument.";
    panel.prompt = @"Open";
    panel.canChooseFiles = YES;
    panel.canChooseDirectories = NO;
    panel.allowsMultipleSelection = NO;

    NSMutableArray<UTType*>* types = [NSMutableArray array];
    for (NSString* ext in instrumentExtensions())
        if (UTType* t = [UTType typeWithFilenameExtension:ext])
            [types addObject:t];
    panel.allowedContentTypes = types;

    NSString* current = _plugin ? ns(_plugin->requestedPath()) : @"";
    NSString* dir = current.length ? current.stringByDeletingLastPathComponent
                                   : [[[NSUserDefaults alloc] initWithSuiteName:kDefaultsSuite] stringForKey:@"lastDirectory"];
    if (dir.length)
        panel.directoryURL = [NSURL fileURLWithPath:dir isDirectory:YES];

    __weak SFPRootView* weakSelf = self;
    void (^done)(NSModalResponse) = ^(NSModalResponse r) {
        if (r == NSModalResponseOK && panel.URL)
            [weakSelf loadPath:panel.URL.path];
    };
    if (self.window)
        [panel beginSheetModalForWindow:self.window completionHandler:done];
    else
        done([panel runModal]);
}

- (void)reload:(id)sender
{
    if (_plugin)
        _plugin->reloadFile();
}

- (void)unload:(id)sender
{
    if (_plugin) {
        _plugin->unloadFile();
        announce(@"Instrument unloaded");
    }
}

- (void)showInFinder:(id)sender
{
    if (!_plugin || _plugin->requestedPath().empty())
        return;
    [[NSWorkspace sharedWorkspace] activateFileViewerSelectingURLs:@[ [NSURL fileURLWithPath:ns(_plugin->requestedPath())] ]];
}

- (void)stepFile:(int)dir
{
    if (!_plugin || _plugin->requestedPath().empty())
        return;
    NSString* current = ns(_plugin->requestedPath());
    NSString* folder = current.stringByDeletingLastPathComponent;
    NSArray<NSString*>* names = [[NSFileManager defaultManager] contentsOfDirectoryAtPath:folder error:nil];
    NSMutableArray<NSString*>* files = [NSMutableArray array];
    for (NSString* n in names)
        if ([instrumentExtensions() containsObject:n.pathExtension.lowercaseString] && ![n hasPrefix:@"."])
            [files addObject:n];
    [files sortUsingSelector:@selector(localizedStandardCompare:)];
    if (files.count == 0)
        return;
    if (files.count == 1) {
        announce(@"This is the only instrument file in the folder");
        return;
    }
    NSUInteger i = [files indexOfObject:current.lastPathComponent];
    NSUInteger next = i == NSNotFound ? 0 : (i + files.count + (NSUInteger)(dir > 0 ? 1 : files.count - 1)) % files.count;
    [self loadPath:[folder stringByAppendingPathComponent:files[next]]];
}

- (void)previousFile:(id)sender
{
    [self stepFile:-1];
}

- (void)nextFile:(id)sender
{
    [self stepFile:1];
}

- (void)selectPreset:(NSInteger)i
{
    if (!_plugin || _presetCount == 0)
        return;
    i = std::clamp<NSInteger>(i, 0, _presetCount - 1);
    [self userChanged:kPreset value:(double)i phase:kSingle];
    [_presetPopup selectItemAtIndex:i];
}

- (void)presetChosen:(NSPopUpButton*)p
{
    [self selectPreset:p.indexOfSelectedItem];
}

- (void)previousPreset:(id)sender
{
    NSInteger i = (NSInteger)std::lround(_plugin->param(kPreset));
    if (i <= 0) {
        NSBeep();
        return;
    }
    [self selectPreset:i - 1];
    announce(_presetPopup.titleOfSelectedItem);
}

- (void)nextPreset:(id)sender
{
    NSInteger i = (NSInteger)std::lround(_plugin->param(kPreset));
    if (i + 1 >= _presetCount) {
        NSBeep();
        return;
    }
    [self selectPreset:i + 1];
    announce(_presetPopup.titleOfSelectedItem);
}

#pragma mark - Recent files

- (NSArray<NSString*>*)recentFiles
{
    static dispatch_once_t migrated;
    dispatch_once(&migrated, ^{
        NSUserDefaults* defaults = [[NSUserDefaults alloc] initWithSuiteName:kDefaultsSuite];
        NSUserDefaults* old = [[NSUserDefaults alloc] initWithSuiteName:@"com.sfplayer.sf-player"];
        for (NSString* key in @[ @"recentFiles", @"lastDirectory", @"selectedTab" ])
            if (![defaults objectForKey:key] && [old objectForKey:key])
                [defaults setObject:[old objectForKey:key] forKey:key];
    });
    NSArray* a = [[[NSUserDefaults alloc] initWithSuiteName:kDefaultsSuite] arrayForKey:@"recentFiles"];
    return [a isKindOfClass:[NSArray class]] ? a : @[];
}

- (void)addRecent:(NSString*)path
{
    NSMutableArray* a = [[self recentFiles] mutableCopy];
    [a removeObject:path];
    [a insertObject:path atIndex:0];
    while ((NSInteger)a.count > kMaxRecent)
        [a removeLastObject];
    [[[NSUserDefaults alloc] initWithSuiteName:kDefaultsSuite] setObject:a forKey:@"recentFiles"];
    [self rebuildRecentMenu];
}

- (void)rebuildRecentMenu
{
    [_recentButton removeAllItems];
    [_recentButton addItemWithTitle:@"Recent"]; // title item of a pull-down
    NSArray<NSString*>* recent = [self recentFiles];
    for (NSString* p in recent) {
        NSMenuItem* item = [[NSMenuItem alloc] initWithTitle:p.lastPathComponent action:@selector(openRecent:) keyEquivalent:@""];
        item.target = self;
        item.representedObject = p;
        item.toolTip = p;
        [_recentButton.menu addItem:item];
    }
    if (recent.count == 0) {
        NSMenuItem* none = [[NSMenuItem alloc] initWithTitle:@"No recent instruments" action:nil keyEquivalent:@""];
        none.enabled = NO;
        [_recentButton.menu addItem:none];
    }
}

- (void)openRecent:(NSMenuItem*)item
{
    [self loadPath:item.representedObject];
}

#pragma mark - Drag and drop

- (NSURL*)droppedURL:(id<NSDraggingInfo>)info
{
    NSArray<NSURL*>* urls = [info.draggingPasteboard readObjectsForClasses:@[ [NSURL class] ] options:@{ NSPasteboardURLReadingFileURLsOnlyKey : @YES }];
    for (NSURL* u in urls)
        if (isInstrumentURL(u))
            return u;
    return nil;
}

- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)info
{
    return [self droppedURL:info] ? NSDragOperationCopy : NSDragOperationNone;
}

- (BOOL)performDragOperation:(id<NSDraggingInfo>)info
{
    NSURL* u = [self droppedURL:info];
    if (!u)
        return NO;
    [self loadPath:u.path];
    return YES;
}

@end

#pragma mark - C++ bridge

namespace sfp {

namespace {

class MacGui final : public Gui {
public:
    explicit MacGui(Plugin& p) { view_ = [[SFPRootView alloc] initWithPlugin:&p]; }

    ~MacGui() override
    {
        [view_ shutdown];
        [view_ removeFromSuperview];
        view_ = nil;
    }

    bool attach(void* parent) override
    {
        NSView* p = (__bridge NSView*)parent;
        if (!p)
            return false;
        [p addSubview:view_];
        view_.frame = NSMakeRect(0, 0, kWidth, kHeight);
        return true;
    }

    void setVisible(bool visible) override { view_.hidden = !visible; }

private:
    SFPRootView* view_;
};

} // namespace

Gui* Gui::create(Plugin& plugin)
{
    return new MacGui(plugin);
}

void Gui::preferredSize(uint32_t* w, uint32_t* h)
{
    *w = (uint32_t)kWidth;
    *h = (uint32_t)kHeight;
}

} // namespace sfp
