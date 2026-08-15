#import "UiPreferences.h"

#import <dispatch/dispatch.h>

@interface CMUiPreferences ()
- (NSString*)defaultsKeyForDock:(ui::PanelDock)dock;
@end

@implementation CMUiPreferences

+ (instancetype)sharedPreferences {
    static CMUiPreferences* instance = nil;
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
      instance = [[CMUiPreferences alloc] init];
    });
    return instance;
}

- (NSString*)defaultsKeyForDock:(ui::PanelDock)dock {
    const std::string key = ui::LastActiveTabPreferenceKey(dock);
    return [NSString stringWithUTF8String:key.c_str()];
}

- (void)setLastActiveIdentifier:(NSString*)identifier
                         forDock:(ui::PanelDock)dock {
    [NSUserDefaults.standardUserDefaults
        setObject:identifier
           forKey:[self defaultsKeyForDock:dock]];
}

- (nullable NSString*)lastActiveIdentifierForDock:(ui::PanelDock)dock {
    return [NSUserDefaults.standardUserDefaults
        stringForKey:[self defaultsKeyForDock:dock]];
}

@end
