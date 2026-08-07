#include "House.h"
#include "CommonRegexes.h"
#include "Log.h"
#include "ParserHelpers.h"

CK3::House::House(std::istream& theStream, long long housID): houseID(housID)
{
	registerKeys();
	parseStream(theStream);
	clearRegisteredKeywords();
}

void CK3::House::registerKeys()
{
	registerKeyword("key", [this](std::istream& theStream) {
		key = commonItems::getString(theStream);
	});
	registerKeyword("name", [this](const std::string&, std::istream& theStream) {
		name = commonItems::singleString(theStream).getString();
	});
	const auto parseLocalizedName = [this](const std::string&, std::istream& theStream) {
		localizedName = commonItems::singleString(theStream).getString();
	};
	registerKeyword("localized_name", parseLocalizedName);
	// CK3 1.19+: rakaly doesn't know the localized_name token, so melted saves write this instead.
	registerKeyword("__unknown_0xcd3", parseLocalizedName);
	const auto parsePrefix = [this](const std::string&, std::istream& theStream) {
		prefix = commonItems::singleString(theStream).getString();
	};
	registerKeyword("prefix", parsePrefix);
	// Same 1.19 melt gap for house name prefixes ("dynnp_de", etc.).
	registerKeyword("__unknown_0xccd", parsePrefix);
	registerKeyword("dynasty", [this](const std::string&, std::istream& theStream) {
		dynasty = std::make_pair(commonItems::singleLlong(theStream).getLlong(), nullptr);
	});
	registerKeyword("head_of_house", [this](const std::string&, std::istream& theStream) {
		houseHead = std::make_pair(commonItems::singleLlong(theStream).getLlong(), nullptr);
	});
	registerRegex(commonItems::catchallRegex, commonItems::ignoreItem);
}
