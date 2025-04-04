#import <Carbon/Carbon.h>
#include <unistd.h>
void get_my_path(char s[PATH_MAX])
{
    CFBundleRef mainBundle = CFBundleGetMainBundle();
    CFURLRef executableURL = CFBundleCopyExecutableURL(mainBundle);
    CFStringRef executablePathString = CFURLCopyFileSystemPath(executableURL, kCFURLPOSIXPathStyle);
    CFRelease(executableURL);
    CFStringGetFileSystemRepresentation(executablePathString, s, PATH_MAX-1);
    CFRelease(executablePathString);
 char *x;
    x = strrchr(s, '/');
    if(x) x[1] = 0;
}
