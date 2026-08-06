/*
  SCDropdownButton.m
  按钮风格下拉：黑色半透明圆角，展开列表贴在按钮正下方。
*/

#import "SCDropdownButton.h"

static __weak SCDropdownButton *gOpenDropdown = nil;

@interface SCDropdownButton ()
@property (nonatomic, strong) UIButton *triggerButton;
@property (nonatomic, strong) UIView *panel;
@property (nonatomic, strong) UIControl *dismissOverlay;
@property (nonatomic, assign) BOOL open;
@end

@implementation SCDropdownButton

- (instancetype)initWithPrefix:(NSString *)prefix
                       options:(NSArray<NSString *> *)options
                 selectedIndex:(NSInteger)selectedIndex {
    self = [super initWithFrame:CGRectZero];
    if (self) {
        _titlePrefix = [prefix copy] ?: @"";
        _options = [options copy] ?: @[];
        _selectedIndex = MAX(0, MIN(selectedIndex, (NSInteger)_options.count - 1));
        _enabled = YES;
        _panelAlignment = SCDropdownPanelAlignmentTrailing;
        self.translatesAutoresizingMaskIntoConstraints = NO;
        [self buildTrigger];
        [self refreshTitle];
    }
    return self;
}

- (void)buildTrigger {
    self.triggerButton = [UIButton buttonWithType:UIButtonTypeSystem];
    self.triggerButton.translatesAutoresizingMaskIntoConstraints = NO;
    self.triggerButton.titleLabel.font = [UIFont monospacedDigitSystemFontOfSize:14 weight:UIFontWeightSemibold];
    self.triggerButton.backgroundColor = [[UIColor blackColor] colorWithAlphaComponent:0.45];
    [self.triggerButton setTitleColor:UIColor.whiteColor forState:UIControlStateNormal];
    self.triggerButton.layer.cornerRadius = 8;
    self.triggerButton.contentEdgeInsets = UIEdgeInsetsMake(8, 10, 8, 10);
    [self.triggerButton addTarget:self action:@selector(toggle) forControlEvents:UIControlEventTouchUpInside];
    [self addSubview:self.triggerButton];

    [NSLayoutConstraint activateConstraints:@[
        [self.triggerButton.leadingAnchor constraintEqualToAnchor:self.leadingAnchor],
        [self.triggerButton.trailingAnchor constraintEqualToAnchor:self.trailingAnchor],
        [self.triggerButton.topAnchor constraintEqualToAnchor:self.topAnchor],
        [self.triggerButton.bottomAnchor constraintEqualToAnchor:self.bottomAnchor],
        [self.heightAnchor constraintEqualToConstant:36],
    ]];
}

- (void)refreshTitle {
    NSString *title;
    if (self.fixedTriggerTitle.length > 0) {
        title = [NSString stringWithFormat:@"%@ ▾", self.fixedTriggerTitle];
    } else {
        NSString *opt = @"";
        if (self.selectedIndex >= 0 && self.selectedIndex < (NSInteger)self.options.count) {
            opt = self.options[self.selectedIndex];
        }
        title = self.titlePrefix.length
            ? [NSString stringWithFormat:@"%@:%@ ▾", self.titlePrefix, opt]
            : [NSString stringWithFormat:@"%@ ▾", opt];
    }
    [self.triggerButton setTitle:title forState:UIControlStateNormal];
    [self invalidateIntrinsicContentSize];
}

- (void)setFixedTriggerTitle:(NSString *)fixedTriggerTitle {
    _fixedTriggerTitle = [fixedTriggerTitle copy];
    [self refreshTitle];
}

- (CGSize)intrinsicContentSize {
    return self.triggerButton.intrinsicContentSize;
}

- (void)setSelectedIndex:(NSInteger)selectedIndex {
    if (self.options.count == 0) {
        _selectedIndex = 0;
    } else {
        _selectedIndex = MAX(0, MIN(selectedIndex, (NSInteger)self.options.count - 1));
    }
    [self refreshTitle];
}

- (void)setEnabled:(BOOL)enabled {
    _enabled = enabled;
    self.triggerButton.enabled = enabled;
    self.alpha = enabled ? 1.0 : 0.45;
    if (!enabled) {
        [self dismiss];
    }
}

- (void)setOptions:(NSArray<NSString *> *)options {
    _options = [options copy] ?: @[];
    if (self.selectedIndex >= (NSInteger)_options.count) {
        self.selectedIndex = MAX(0, (NSInteger)_options.count - 1);
    }
    [self refreshTitle];
    if (self.open) {
        [self dismiss];
        [self show];
    }
}

- (UIButton *)makeOptionButton:(NSString *)title selected:(BOOL)selected {
    UIButton *b = [UIButton buttonWithType:UIButtonTypeSystem];
    b.translatesAutoresizingMaskIntoConstraints = NO;
    [b setTitle:title forState:UIControlStateNormal];
    b.titleLabel.font = [UIFont monospacedDigitSystemFontOfSize:14 weight:UIFontWeightSemibold];
    b.backgroundColor = selected
        ? [[UIColor colorWithWhite:0.25 alpha:1] colorWithAlphaComponent:0.92]
        : [[UIColor blackColor] colorWithAlphaComponent:0.82];
    [b setTitleColor:UIColor.whiteColor forState:UIControlStateNormal];
    b.contentEdgeInsets = UIEdgeInsetsMake(8, 12, 8, 12);
    b.contentHorizontalAlignment = UIControlContentHorizontalAlignmentLeft;
    [b.heightAnchor constraintEqualToConstant:36].active = YES;
    return b;
}

- (void)toggle {
    if (!self.enabled) return;
    if (self.open) {
        [self dismiss];
    } else {
        [self show];
    }
}

- (void)show {
    if (self.open || self.options.count == 0) return;
    if (gOpenDropdown && gOpenDropdown != self) {
        [gOpenDropdown dismiss];
    }

    UIView *host = self.window ?: self.superview;
    if (!host) return;

    self.dismissOverlay = [[UIControl alloc] initWithFrame:host.bounds];
    self.dismissOverlay.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    self.dismissOverlay.backgroundColor = [UIColor clearColor];
    [self.dismissOverlay addTarget:self action:@selector(dismiss) forControlEvents:UIControlEventTouchUpInside];
    [host addSubview:self.dismissOverlay];

    self.panel = [[UIView alloc] init];
    self.panel.translatesAutoresizingMaskIntoConstraints = NO;
    self.panel.backgroundColor = [[UIColor blackColor] colorWithAlphaComponent:0.88];
    self.panel.layer.cornerRadius = 8;
    self.panel.layer.masksToBounds = YES;
    self.panel.layer.borderWidth = 1.0;
    self.panel.layer.borderColor = [[UIColor whiteColor] colorWithAlphaComponent:0.25].CGColor;

    UIStackView *stack = [[UIStackView alloc] init];
    stack.axis = UILayoutConstraintAxisVertical;
    stack.spacing = 1;
    stack.translatesAutoresizingMaskIntoConstraints = NO;
    [self.panel addSubview:stack];

    for (NSInteger i = 0; i < (NSInteger)self.options.count; i++) {
        BOOL sel = (i == self.selectedIndex);
        NSString *name = self.options[i];
        if (sel) name = [name stringByAppendingString:@"  ✓"];
        UIButton *row = [self makeOptionButton:name selected:sel];
        row.tag = i;
        [row addTarget:self action:@selector(optionTapped:) forControlEvents:UIControlEventTouchUpInside];
        [stack addArrangedSubview:row];
    }

    [host addSubview:self.panel];

    CGRect triggerInHost = [self.triggerButton convertRect:self.triggerButton.bounds toView:host];
    CGFloat panelW = MAX(CGRectGetWidth(triggerInHost), 140);
    NSMutableArray<NSLayoutConstraint *> *cs = [NSMutableArray arrayWithArray:@[
        [stack.leadingAnchor constraintEqualToAnchor:self.panel.leadingAnchor],
        [stack.trailingAnchor constraintEqualToAnchor:self.panel.trailingAnchor],
        [stack.topAnchor constraintEqualToAnchor:self.panel.topAnchor],
        [stack.bottomAnchor constraintEqualToAnchor:self.panel.bottomAnchor],
        [self.panel.topAnchor constraintEqualToAnchor:host.topAnchor constant:CGRectGetMaxY(triggerInHost) + 4],
        [self.panel.widthAnchor constraintGreaterThanOrEqualToConstant:panelW],
    ]];
    if (self.panelAlignment == SCDropdownPanelAlignmentLeading) {
        [cs addObject:[self.panel.leadingAnchor constraintEqualToAnchor:host.leadingAnchor
                                                              constant:CGRectGetMinX(triggerInHost)]];
    } else {
        [cs addObject:[self.panel.trailingAnchor constraintEqualToAnchor:host.leadingAnchor
                                                               constant:CGRectGetMaxX(triggerInHost)]];
    }
    [NSLayoutConstraint activateConstraints:cs];

    self.panel.transform = CGAffineTransformMakeTranslation(0, -6);
    self.panel.alpha = 0;
    [UIView animateWithDuration:0.15 animations:^{
        self.panel.alpha = 1;
        self.panel.transform = CGAffineTransformIdentity;
    }];

    self.open = YES;
    gOpenDropdown = self;
}

- (void)optionTapped:(UIButton *)sender {
    NSInteger idx = sender.tag;
    if (idx < 0 || idx >= (NSInteger)self.options.count) return;
    self.selectedIndex = idx;
    NSString *title = self.options[idx];
    [self dismiss];
    if (self.selectionHandler) {
        self.selectionHandler(idx, title);
    }
}

- (void)dismiss {
    if (!self.open) return;
    UIView *panel = self.panel;
    UIControl *overlay = self.dismissOverlay;
    self.panel = nil;
    self.dismissOverlay = nil;
    self.open = NO;
    if (gOpenDropdown == self) gOpenDropdown = nil;

    [UIView animateWithDuration:0.12 animations:^{
        panel.alpha = 0;
        panel.transform = CGAffineTransformMakeTranslation(0, -4);
    } completion:^(__unused BOOL finished) {
        [panel removeFromSuperview];
        [overlay removeFromSuperview];
    }];
}

- (void)didMoveToWindow {
    [super didMoveToWindow];
    if (!self.window) {
        [self dismiss];
    }
}

@end
