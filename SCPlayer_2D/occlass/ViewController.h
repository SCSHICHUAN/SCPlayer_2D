//
//  ViewController.h
//  SCFFmpeg
//
//  Created by 石川 on 2019/5/18.
//  Copyright © 2019 石川. All rights reserved.
//

#import <UIKit/UIKit.h>
#import <Foundation/Foundation.h>
#include <libavformat/avformat.h>

static int decode_write_frame(AVFrame *frame);


@interface ViewController : UIViewController

@end

