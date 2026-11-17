// blz (16 Nov 2026): a slim build (DM_COPY_ROMFS=NO, <App>/romfs.id) fetches its game file image by itself on the first start, so one
// tap on the install page is enough: install, open, wait for the download once, play. Included by main.m only (it uses main.m's
// romfs_image_complete / romfs_identity_accepts), so the Xcode project needs no new source file.
//
// <App>/dm/blz_data.txt (Gen/dm/blz_data.txt at build time; '#' starts a comment):
//   line 1  https URL of the file that holds the image
//   line 2  size of the image in bytes
//   line 3  (optional) byte offset of the image inside that file - an image stored uncompressed in a zip/IPA can be served from it
// Without that file: BLZ_DATA_URL / BLZ_DATA_SIZE / BLZ_DATA_OFFSET from dm/env.txt (the build script's --env-file).
// The download goes to Documents/romfs.img.dl with HTTP Range requests, continues where it stopped (app closed, network lost),
// checks free space first (roomFor: the rest of the image + 500 MB must fit, else a message with the numbers and no download), and is renamed to Documents/romfs.img only when the image is complete and has the identity in romfs.id.
#import <UIKit/UIKit.h>

typedef struct { char url[1024]; long long size, offset; } blz_data_spec;

static int blz_data_spec_read(blz_data_spec *s) {
    NSString *p = [[NSBundle.mainBundle.resourcePath stringByAppendingPathComponent:@"dm"] stringByAppendingPathComponent:@"blz_data.txt"];
    NSString *t = [NSString stringWithContentsOfFile:p encoding:NSUTF8StringEncoding error:nil];
    if (!t) {   // or the same three values from an env file (the build script's --env-file becomes dm/env.txt): BLZ_DATA_URL, BLZ_DATA_SIZE, BLZ_DATA_OFFSET
        const char *u = getenv("BLZ_DATA_URL"), *z = getenv("BLZ_DATA_SIZE"), *o = getenv("BLZ_DATA_OFFSET");
        if (!u || !z || strncmp(u, "https://", 8) || strlen(u) >= sizeof s->url) return 0;
        memset(s, 0, sizeof *s); strlcpy(s->url, u, sizeof s->url); s->size = atoll(z); s->offset = o ? atoll(o) : 0;
        return s->size > 0 && s->offset >= 0;
    }
    NSMutableArray<NSString *> *rows = [NSMutableArray array];
    for (NSString *raw in [t componentsSeparatedByCharactersInSet:NSCharacterSet.newlineCharacterSet]) {
        NSString *l = [raw stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
        if (l.length && ![l hasPrefix:@"#"]) [rows addObject:l];
    }
    if (rows.count < 2 || ![rows[0] hasPrefix:@"https://"] || rows[0].length >= sizeof s->url) return 0;
    memset(s, 0, sizeof *s);
    strlcpy(s->url, rows[0].UTF8String, sizeof s->url);
    s->size = rows[1].longLongValue; s->offset = rows.count > 2 ? rows[2].longLongValue : 0;
    return s->size > 0 && s->offset >= 0;
}

@interface BlzDataVC : UIViewController <NSURLSessionDataDelegate>
- (instancetype)initWithSpec:(blz_data_spec)spec done:(void (^)(void))done;
- (void)setRoomTest;
@end

@implementation BlzDataVC {
    blz_data_spec _spec; void (^_done)(void);
    NSURLSession *_session; NSURLSessionDataTask *_task;
    int _fd; long long _have; int _failed, _finished, _responseOK, _noRoom, _roomSaid, _roomTest;
    UILabel *_title, *_status; UIProgressView *_bar;
    CFAbsoluteTime _lastUI, _rateT; long long _rateBytes; double _rate;
    NSString *_tmp, *_img;
}
- (instancetype)initWithSpec:(blz_data_spec)spec done:(void (^)(void))done {
    if ((self = [super initWithNibName:nil bundle:nil])) { _spec = spec; _done = [done copy]; _fd = -1; }
    return self;
}
- (void)setRoomTest { _roomTest = 1; }
- (BOOL)prefersHomeIndicatorAutoHidden { return YES; }
- (void)viewDidLoad {
    [super viewDidLoad];
    self.view.backgroundColor = UIColor.blackColor;
    _title = [UILabel new]; _status = [UILabel new]; _bar = [[UIProgressView alloc] initWithProgressViewStyle:UIProgressViewStyleDefault];
    _title.text = [NSBundle.mainBundle objectForInfoDictionaryKey:@"CFBundleDisplayName"] ?: @"Game";
    _title.font = [UIFont systemFontOfSize:26 weight:UIFontWeightSemibold];
    _status.font = [UIFont monospacedDigitSystemFontOfSize:15 weight:UIFontWeightRegular]; _status.numberOfLines = 0;
    for (UILabel *l in @[_title, _status]) { l.textColor = UIColor.whiteColor; l.textAlignment = NSTextAlignmentCenter; l.translatesAutoresizingMaskIntoConstraints = NO; [self.view addSubview:l]; }
    _bar.translatesAutoresizingMaskIntoConstraints = NO; [self.view addSubview:_bar];
    UILayoutGuide *g = self.view.safeAreaLayoutGuide;
    [NSLayoutConstraint activateConstraints:@[
        [_bar.centerYAnchor constraintEqualToAnchor:g.centerYAnchor], [_bar.centerXAnchor constraintEqualToAnchor:g.centerXAnchor],
        [_bar.widthAnchor constraintEqualToAnchor:g.widthAnchor multiplier:0.7],
        [_title.bottomAnchor constraintEqualToAnchor:_bar.topAnchor constant:-24], [_title.centerXAnchor constraintEqualToAnchor:g.centerXAnchor],
        [_title.widthAnchor constraintEqualToAnchor:g.widthAnchor multiplier:0.9],
        [_status.topAnchor constraintEqualToAnchor:_bar.bottomAnchor constant:20], [_status.centerXAnchor constraintEqualToAnchor:g.centerXAnchor],
        [_status.widthAnchor constraintEqualToAnchor:g.widthAnchor multiplier:0.9]]];
    NSString *doc = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES)[0];
    _tmp = [doc stringByAppendingPathComponent:@"romfs.img.dl"]; _img = [doc stringByAppendingPathComponent:@"romfs.img"];
    NSURLSessionConfiguration *c = NSURLSessionConfiguration.ephemeralSessionConfiguration;
    c.waitsForConnectivity = YES; c.timeoutIntervalForRequest = 60; c.timeoutIntervalForResource = 7 * 24 * 3600; c.URLCache = nil;
    NSOperationQueue *q = [NSOperationQueue new]; q.maxConcurrentOperationCount = 1;
    _session = [NSURLSession sessionWithConfiguration:c delegate:self delegateQueue:q];
    UIApplication.sharedApplication.idleTimerDisabled = YES;   // a 20 GB download must not stop for the auto-lock
    fprintf(stderr, "blz data: download screen (%lld bytes, offset %lld)\n", _spec.size, _spec.offset);
    [self begin];
}
static NSString *blz_gb(long long b) { return [NSString stringWithFormat:@"%.1f", (double)b / 1e9]; }
- (void)say:(NSString *)s progress:(float)p {
    dispatch_async(dispatch_get_main_queue(), ^{ self->_status.text = s; self->_bar.progress = p; });
}
- (void)retryIn:(double)sec { dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(sec * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{ [self begin]; }); }
// Free-room guard: the bytes still to fetch plus a 500 MB margin must fit before anything is asked for. The reading is the volume's
// capacity for important usage (what iOS can give an app the user is waiting on: free space plus what it may clear); after a write that
// failed for lack of room (_noRoom) the plain free space counts instead, so a too-hopeful reading cannot make the download loop.
// Short of room: a plain message with the numbers, nothing is downloaded; begin tries again every 5 s, so it starts once room is there.
- (BOOL)roomFor:(long long)left free:(long long *)freeOut {
    NSNumber *imp = nil, *plain = nil; NSURL *dir = [NSURL fileURLWithPath:_tmp.stringByDeletingLastPathComponent];
    [dir getResourceValue:&imp forKey:NSURLVolumeAvailableCapacityForImportantUsageKey error:nil];
    [dir getResourceValue:&plain forKey:NSURLVolumeAvailableCapacityKey error:nil];
    NSNumber *freeN = _noRoom && plain ? plain : imp ?: plain;
    long long need = left + (500ll << 20), have = freeN ? freeN.longLongValue : 0;
    if (freeOut) *freeOut = have;
    if (!freeN || have >= need) return YES;
    long long shortBy = need - have;
    if (!_roomSaid) fprintf(stderr, "blz data: not enough room: needs %lld, free %lld (important usage %lld, plain %lld); nothing downloaded\n", need, have, imp.longLongValue, plain.longLongValue);
    _roomSaid = 1;
    NSString *up = shortBy >= 1000000000ll ? [NSString stringWithFormat:@"%.0f GB", ceil((double)shortBy / 1e9)] : [NSString stringWithFormat:@"%.0f MB", ceil((double)shortBy / 1e8) * 100];
    [self say:[NSString stringWithFormat:@"Not enough room on this %@.\nNeeds %@ GB, %@ GB free.\nFree up %@ and open the game again.", UIDevice.currentDevice.model, blz_gb(need), blz_gb(have), up]
     progress:(float)((double)_have / (double)_spec.size)];
    return NO;
}
// One attempt: open the partial file, check the free space, ask for the rest
- (void)begin {
    if (_finished) return;
    if (_roomTest) {   // BLZ_DATA_ROOM_TEST=<bytes> (launch environment): only the free-room guard for an image of that size; no file is opened, nothing is downloaded
        long long f = 0; if ([self roomFor:_spec.size free:&f]) { fprintf(stderr, "blz data: room test: %lld bytes would fit (free %lld)\n", _spec.size, f); [self say:[NSString stringWithFormat:@"Room test: %@ GB would fit, %@ GB free.", blz_gb(_spec.size), blz_gb(f)] progress:0]; }
        return;
    }
    if (_fd >= 0) { close(_fd); _fd = -1; }
    _fd = open(_tmp.fileSystemRepresentation, O_WRONLY | O_CREAT, 0644);
    if (_fd < 0) { [self say:[NSString stringWithFormat:@"Cannot write the game files (error %d).", errno] progress:0]; [self retryIn:5]; return; }
    [[NSURL fileURLWithPath:_tmp] setResourceValue:@YES forKey:NSURLIsExcludedFromBackupKey error:nil];
    struct stat st; _have = fstat(_fd, &st) == 0 ? st.st_size : 0;
    if (_have > _spec.size) { ftruncate(_fd, 0); _have = 0; }
    lseek(_fd, _have, SEEK_SET);
    if (_have == _spec.size) { [self finish]; return; }
    long long freeB = 0;
    if (![self roomFor:_spec.size - _have free:&freeB]) { [self retryIn:5]; return; }
    NSMutableURLRequest *r = [NSMutableURLRequest requestWithURL:[NSURL URLWithString:@(_spec.url)]];
    r.cachePolicy = NSURLRequestReloadIgnoringLocalAndRemoteCacheData;
    [r setValue:[NSString stringWithFormat:@"bytes=%lld-%lld", _spec.offset + _have, _spec.offset + _spec.size - 1] forHTTPHeaderField:@"Range"];
    [r setValue:@"identity" forHTTPHeaderField:@"Accept-Encoding"];
    _responseOK = 0; _rateT = CFAbsoluteTimeGetCurrent(); _rateBytes = _have;
    fprintf(stderr, "blz data: request from byte %lld of %lld (free %lld)\n", _have, _spec.size, freeB);
    [self say:[NSString stringWithFormat:@"Downloading the game (only once): %@ of %@ GB", blz_gb(_have), blz_gb(_spec.size)] progress:(float)((double)_have / (double)_spec.size)];
    _task = [_session dataTaskWithRequest:r]; [_task resume];
}
- (void)URLSession:(NSURLSession *)s dataTask:(NSURLSessionDataTask *)t didReceiveResponse:(NSURLResponse *)resp completionHandler:(void (^)(NSURLSessionResponseDisposition))h {
    NSHTTPURLResponse *r = (NSHTTPURLResponse *)resp; long long want = _spec.offset + _have;
    NSString *cr = [r valueForHTTPHeaderField:@"Content-Range"];   // "bytes <first>-<last>/<total>"
    BOOL ok = r.statusCode == 206 && [cr hasPrefix:[NSString stringWithFormat:@"bytes %lld-", want]];
    if (!ok && r.statusCode == 200 && want == 0 && r.expectedContentLength == _spec.size) ok = YES;   // a server without Range, asked from the start
    if (!ok) { fprintf(stderr, "[RED] blz data: unexpected answer %ld (Content-Range %s) for byte %lld\n", (long)r.statusCode, cr.UTF8String ?: "-", want); h(NSURLSessionResponseCancel); return; }
    _responseOK = 1; h(NSURLSessionResponseAllow);
}
- (void)URLSession:(NSURLSession *)s dataTask:(NSURLSessionDataTask *)t didReceiveData:(NSData *)d {
    if (!_responseOK || _fd < 0) return;
    __block BOOL bad = NO;
    [d enumerateByteRangesUsingBlock:^(const void *bytes, NSRange range, BOOL *stop) {
        size_t off = 0;
        while (off < range.length) {
            ssize_t n = write(self->_fd, (const char *)bytes + off, range.length - off);
            if (n < 0) { if (errno == EINTR) continue; if (errno == ENOSPC) self->_noRoom = 1; bad = YES; *stop = YES; return; }
            off += (size_t)n;
        }
    }];
    if (bad) { fprintf(stderr, "[RED] blz data: write failed (errno %d) at byte %lld\n", errno, _have); _responseOK = 0; [t cancel]; return; }
    _have += (long long)d.length;
    CFAbsoluteTime now = CFAbsoluteTimeGetCurrent();
    if (now - _lastUI < 0.5) return;
    _lastUI = now;
    if (now - _rateT >= 3.0) { _rate = (double)(_have - _rateBytes) / (now - _rateT); _rateT = now; _rateBytes = _have; }
    NSString *eta = @"";
    if (_rate > 1e4) { double left = (double)(_spec.size - _have) / _rate; eta = left >= 90 ? [NSString stringWithFormat:@" - about %.0f min left", left / 60] : @" - about 1 min left"; }
    [self say:[NSString stringWithFormat:@"Downloading the game (only once): %@ of %@ GB%@%@\nKeep this app open. If it stops, open it again: it continues where it was.",
               blz_gb(_have), blz_gb(_spec.size), _rate > 0 ? [NSString stringWithFormat:@" - %.1f MB/s", _rate / 1e6] : @"", eta]
     progress:(float)((double)_have / (double)_spec.size)];
}
- (void)URLSession:(NSURLSession *)s task:(NSURLSessionTask *)t didCompleteWithError:(NSError *)e {
    dispatch_async(dispatch_get_main_queue(), ^{
        if (self->_finished) return;
        if (self->_fd >= 0) fsync(self->_fd);
        if (self->_have == self->_spec.size) { [self finish]; return; }
        self->_failed++;
        fprintf(stderr, "blz data: stopped at byte %lld of %lld (%s); trying again (%d)\n", self->_have, self->_spec.size, e ? e.localizedDescription.UTF8String : "the answer ended early", self->_failed);
        [self say:[NSString stringWithFormat:@"Download paused at %@ of %@ GB (%@).\nTrying again...", blz_gb(self->_have), blz_gb(self->_spec.size), e.localizedDescription ?: @"the connection ended"]
         progress:(float)((double)self->_have / (double)self->_spec.size)];
        [self retryIn:self->_failed < 5 ? 3 : 10];
    });
}
// The whole image is there: check it, give it its name, start the game
- (void)finish {
    if (_fd >= 0) { fsync(_fd); close(_fd); _fd = -1; }
    [self say:@"Checking the game files..." progress:1];
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        BOOL ok = romfs_image_complete(self->_tmp) && romfs_identity_accepts(self->_tmp);
        dispatch_async(dispatch_get_main_queue(), ^{
            NSFileManager *fm = NSFileManager.defaultManager;
            if (!ok) {   // not the image this build needs (a wrong blz_data.txt, or a damaged file): start over rather than loop on it
                fprintf(stderr, "[RED] blz data: the downloaded image is not the one this build needs; deleted, downloading again\n");
                [fm removeItemAtPath:self->_tmp error:nil];
                [self say:@"The downloaded game files were damaged. Downloading again..." progress:0]; [self retryIn:5]; return;
            }
            NSString *doc = self->_img.stringByDeletingLastPathComponent;   // an older image under any letter case goes: the new one replaces it
            for (NSString *n in [fm contentsOfDirectoryAtPath:doc error:nil]) if ([n caseInsensitiveCompare:@"romfs.img"] == NSOrderedSame) [fm removeItemAtPath:[doc stringByAppendingPathComponent:n] error:nil];
            NSError *err = nil;
            if (![fm moveItemAtPath:self->_tmp toPath:self->_img error:&err]) { fprintf(stderr, "[RED] blz data: rename failed: %s\n", err.localizedDescription.UTF8String); [self retryIn:5]; return; }
            fprintf(stderr, "blz data: image complete (%lld bytes); starting the game\n", self->_spec.size);
            self->_finished = 1; [self->_session finishTasksAndInvalidate];
            self->_status.text = @"Starting the game...";
            if (self->_done) self->_done();
        });
    });
}
@end
