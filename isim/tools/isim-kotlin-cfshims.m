/* What the Kotlin/Native runtime calls in CoreFoundation, for an isim SDK that does not export it yet (isim-kotlin.py
   adds this object to the framework only when the SDK's CFString.h lacks CFStringGetBytes). */
#import <Foundation/Foundation.h>

unsigned int CFStringGetFastestEncoding(CFStringRef s) {
    return 0x0100; /* kCFStringEncodingUnicode */
}

CFIndex CFStringGetBytes(CFStringRef s, CFRange range, unsigned int encoding, UInt8 lossByte,
                         Boolean isExternalRepresentation, UInt8 *buffer, CFIndex maxBufLen, CFIndex *usedBufLen) {
    NSStringEncoding enc;
    switch (encoding) {
        case 0x0600: enc = NSASCIIStringEncoding; break;
        case 0x0201: enc = NSISOLatin1StringEncoding; break;
        case 0x0100: enc = NSUTF16LittleEndianStringEncoding; break;
        default: enc = NSUTF8StringEncoding; break;
    }
    NSString *str = (__bridge NSString *)s;
    NSRange r = NSMakeRange(range.location, range.length);
    if (buffer == NULL) {
        NSData *d = [[str substringWithRange:r] dataUsingEncoding:enc allowLossyConversion:lossByte != 0];
        if (usedBufLen) *usedBufLen = d.length;
        return d ? range.length : 0;
    }
    NSUInteger used = 0;
    NSRange left = NSMakeRange(0, 0);
    [str getBytes:buffer maxLength:maxBufLen usedLength:&used encoding:enc
          options:(lossByte ? NSStringEncodingConversionAllowLossy : 0) range:r remainingRange:&left];
    if (usedBufLen) *usedBufLen = used;
    return range.length - left.length;
}
