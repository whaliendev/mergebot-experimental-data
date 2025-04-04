#import <Carbon/Carbon.h>
#include <unistd.h>
void get_my_path(char *s, size_t maxLen)
{
    CFBundleRef mainBundle = CFBundleGetMainBundle();
    CFURLRef executableURL = CFBundleCopyExecutableURL(mainBundle);
    CFStringRef executablePathString = CFURLCopyFileSystemPath(executableURL, kCFURLPOSIXPathStyle);
    CFRelease(executableURL);
    CFStringGetFileSystemRepresentation(executablePathString, s, maxLen);
    CFRelease(executablePathString);
}
