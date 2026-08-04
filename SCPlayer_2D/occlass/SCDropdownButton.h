/*
  SCDropdownButton.h
  与控件栏按钮同风格的下拉选择：点按钮后在下方展开选项列表。
*/

#import <UIKit/UIKit.h>

NS_ASSUME_NONNULL_BEGIN

typedef NS_ENUM(NSInteger, SCDropdownPanelAlignment) {
    SCDropdownPanelAlignmentTrailing = 0, // 右对齐触发按钮（右侧控件栏）
    SCDropdownPanelAlignmentLeading,      // 左对齐触发按钮（左侧 Anim 等）
};

@interface SCDropdownButton : UIView

@property (nonatomic, copy) NSString *titlePrefix;
@property (nonatomic, copy) NSArray<NSString *> *options;
@property (nonatomic, assign) NSInteger selectedIndex;
@property (nonatomic, assign, getter=isEnabled) BOOL enabled;
@property (nonatomic, assign) SCDropdownPanelAlignment panelAlignment;
@property (nonatomic, copy, nullable) void (^selectionHandler)(NSInteger index, NSString *title);

- (instancetype)initWithPrefix:(NSString *)prefix
                       options:(NSArray<NSString *> *)options
                 selectedIndex:(NSInteger)selectedIndex;

- (void)setSelectedIndex:(NSInteger)selectedIndex;
- (void)dismiss;

@end

NS_ASSUME_NONNULL_END
