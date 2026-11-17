#pragma once
/* isim CoreText: the AAT feature types and selectors (font feature settings: kCTFontFeatureTypeIdentifierKey /
 * kCTFontFeatureSelectorIdentifierKey, UIFontDescriptor.FeatureKey.type / .selector). Values are the font format's. */
enum {
    kAllTypographicFeaturesType = 0, kLigaturesType = 1, kCursiveConnectionType = 2, kLetterCaseType = 3,
    kVerticalSubstitutionType = 4, kLinguisticRearrangementType = 5, kNumberSpacingType = 6, kSmartSwashType = 8,
    kDiacriticsType = 9, kVerticalPositionType = 10, kFractionsType = 11, kOverlappingCharactersType = 13,
    kTypographicExtrasType = 14, kMathematicalExtrasType = 15, kOrnamentSetsType = 16, kCharacterAlternativesType = 17,
    kDesignComplexityType = 18, kStyleOptionsType = 19, kCharacterShapeType = 20, kNumberCaseType = 21,
    kTextSpacingType = 22, kTransliterationType = 23, kAnnotationType = 24, kKanaSpacingType = 25,
    kIdeographicSpacingType = 26, kUnicodeDecompositionType = 27, kRubyKanaType = 28, kCJKSymbolAlternativesType = 29,
    kIdeographicAlternativesType = 30, kCJKVerticalRomanPlacementType = 31, kItalicCJKRomanType = 32,
    kCaseSensitiveLayoutType = 33, kAlternateKanaType = 34, kStylisticAlternativesType = 35,
    kContextualAlternatesType = 36, kLowerCaseType = 37, kUpperCaseType = 38, kLanguageTagType = 39,
    kCJKRomanSpacingType = 103, kLastFeatureType = -1
};
enum { kAllTypeFeaturesOnSelector = 0, kAllTypeFeaturesOffSelector = 1 };
enum {
    kRequiredLigaturesOnSelector = 0, kRequiredLigaturesOffSelector = 1, kCommonLigaturesOnSelector = 2,
    kCommonLigaturesOffSelector = 3, kRareLigaturesOnSelector = 4, kRareLigaturesOffSelector = 5,
    kContextualLigaturesOnSelector = 18, kContextualLigaturesOffSelector = 19, kHistoricalLigaturesOnSelector = 20,
    kHistoricalLigaturesOffSelector = 21
};
enum { kMonospacedNumbersSelector = 0, kProportionalNumbersSelector = 1, kThirdWidthNumbersSelector = 2, kQuarterWidthNumbersSelector = 3 };
enum {
    kNormalPositionSelector = 0, kSuperiorsSelector = 1, kInferiorsSelector = 2, kOrdinalsSelector = 3,
    kScientificInferiorsSelector = 4
};
enum { kNoFractionsSelector = 0, kVerticalFractionsSelector = 1, kDiagonalFractionsSelector = 2 };
enum { kSlashedZeroOnSelector = 4, kSlashedZeroOffSelector = 5 };
enum { kLowerCaseNumbersSelector = 0, kUpperCaseNumbersSelector = 1 };
enum { kCaseSensitiveLayoutOnSelector = 0, kCaseSensitiveLayoutOffSelector = 1, kCaseSensitiveSpacingOnSelector = 2, kCaseSensitiveSpacingOffSelector = 3 };
enum {
    kNoStylisticAlternatesSelector = 0, kStylisticAltOneOnSelector = 2, kStylisticAltOneOffSelector = 3,
    kStylisticAltTwoOnSelector = 4, kStylisticAltTwoOffSelector = 5, kStylisticAltThreeOnSelector = 6,
    kStylisticAltThreeOffSelector = 7, kStylisticAltFourOnSelector = 8, kStylisticAltFourOffSelector = 9,
    kStylisticAltFiveOnSelector = 10, kStylisticAltFiveOffSelector = 11, kStylisticAltSixOnSelector = 12,
    kStylisticAltSixOffSelector = 13, kStylisticAltSevenOnSelector = 14, kStylisticAltSevenOffSelector = 15,
    kStylisticAltEightOnSelector = 16, kStylisticAltEightOffSelector = 17, kStylisticAltNineOnSelector = 18,
    kStylisticAltNineOffSelector = 19, kStylisticAltTenOnSelector = 20, kStylisticAltTenOffSelector = 21
};
enum {
    kContextualAlternatesOnSelector = 0, kContextualAlternatesOffSelector = 1, kSwashAlternatesOnSelector = 2,
    kSwashAlternatesOffSelector = 3, kContextualSwashAlternatesOnSelector = 4, kContextualSwashAlternatesOffSelector = 5
};
enum { kDefaultLowerCaseSelector = 0, kLowerCaseSmallCapsSelector = 1, kLowerCasePetiteCapsSelector = 2 };
enum { kDefaultUpperCaseSelector = 0, kUpperCaseSmallCapsSelector = 1, kUpperCasePetiteCapsSelector = 2 };
