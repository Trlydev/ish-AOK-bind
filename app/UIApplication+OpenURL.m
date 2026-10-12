//
//  UIApplication+OpenURL.m
//  iSH
//
//  Created by Theodore Dubois on 9/23/18.
//

#import "UIApplication+OpenURL.h"

@implementation UIApplication (OpenURL)

+ (void)openURL:(NSString *)url {
    // (URLWithString: throws on nil, and a string that is no URL gives nil.)
    NSURL *parsed = url != nil ? [NSURL URLWithString:url] : nil;
    if (parsed == nil)
        return;
    [[self sharedApplication] openURL:parsed options:@{} completionHandler:nil];
}

@end
