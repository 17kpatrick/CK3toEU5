#include "outWorld.h"
#include "BlockParsing.h"
#include "CommonRegexes.h"
#include "Log.h"
#include "OSCompatibilityLayer.h"
#include "ParserHelpers.h"
#include "src/ck3_world/CoatsOfArms/CoatOfArms.h"
#include "src/ck3_world/CoatsOfArms/Emblem.h"
#include "src/ck3_world/CoatsOfArms/EmblemInstance.h"
#include "src/configuration/configuration.hpp"
#include "src/eu5_world/World.h"
#include <algorithm>
#include <cmath>
#include <format>
#include <fstream>
#include <map>
#include <optional>
#include <ranges>
#include <regex>
#include <set>
#include <sstream>

namespace
{
using output::extractBlockBody;
using output::extractNamedBlocks;
using output::findBlockBody;
using output::slurpFile;
using output::touchesConvertedLand;
using output::trimToSurvivingLand;

void writeUtf8Bom(std::ofstream& output)
{
	output << "\xEF\xBB\xBF";
}

void writeMetadata(const std::filesystem::path& modFolder, const std::string& modName)
{
	std::filesystem::create_directories(modFolder / ".metadata");
	std::ofstream metadata(modFolder / ".metadata" / "metadata.json");
	metadata << "{\n";
	metadata << "\t\"name\": \"" << modName << "\",\n";
	metadata << "\t\"id\": \"\",\n";
	metadata << "\t\"version\": \"0.1.0\",\n";
	metadata << "\t\"supported_game_version\": \"\",\n";
	metadata << "\t\"short_description\": \"Converted CK3 game - created by CK3toEU5 converter\",\n";
	metadata << "\t\"tags\": [\"Alternative History\", \"Map\"],\n";
	metadata << "\t\"relationships\": [],\n";
	metadata << "\t\"game_custom_data\": {\n";
	metadata << "\t\t\"multiplayer_synchronized\": true,\n";
	// Country definitions have to replace vanilla's rather than sit alongside them; EU5 keeps the
	// first definition it reads for a tag, so an added file could never correct one.
	metadata << "\t\t\"replace_paths\": [\n";
	metadata << "\t\t\t\"in_game/setup/countries\"\n";
	metadata << "\t\t]\n";
	metadata << "\t}\n";
	metadata << "}\n";
	metadata.close();
}

void writeColor(std::ofstream& output, const std::optional<commonItems::Color>& color, const std::optional<commonItems::Color>& secondary)
{
	auto [r, g, b] = std::tuple{128, 128, 128};
	if (color)
		std::tie(r, g, b) = color->getRgbComponents();
	output << "\tcolor = rgb { " << r << " " << g << " " << b << " }\n";

	// color2 is the trim the interface draws against the map colour, so it has to read as a
	// different colour. The arms' second tincture is the honest answer; a darkened primary the fallback.
	if (secondary)
	{
		const auto [r2, g2, b2] = secondary->getRgbComponents();
		if (std::abs(r2 - r) + std::abs(g2 - g) + std::abs(b2 - b) > 60)
		{
			output << "\tcolor2 = rgb { " << r2 << " " << g2 << " " << b2 << " }\n";
			return;
		}
	}
	output << "\tcolor2 = rgb { " << r / 3 << " " << g / 3 << " " << b / 3 << " }\n";
}

// Country definitions. This folder is replaced rather than added to, because EU5 keeps the first
// definition it reads for a tag and ignores the rest - so a converted realm reusing a vanilla tag
// would otherwise keep vanilla's colour and culture. Replacing also lets vanilla's definitions be
// rewritten on the way through: a country whose entire homeland converted no longer exists at the
// bookmark, and the game asks for exactly one thing in that case ("add 'is_historic = yes' to the
// definition"), which turns some eighteen thousand lines of load-time complaints into silence while
// leaving the country available for its historic rulers and as a formable nation.
void writeCountryDefinitions(const std::filesystem::path& modFolder,
	 const EU5::World& world,
	 const std::filesystem::path& eu5GameFolder,
	 const std::set<std::string>& keptTags)
{
	const auto folder = modFolder / "in_game" / "setup" / "countries";
	std::filesystem::create_directories(folder);

	const auto vanillaFolder = eu5GameFolder / "in_game" / "setup" / "countries";
	auto historicCount = 0;
	if (std::filesystem::exists(vanillaFolder))
		for (const auto& file: std::filesystem::directory_iterator(vanillaFolder))
		{
			if (file.path().extension() != ".txt")
				continue;
			std::ofstream copy(folder / file.path().filename());
			writeUtf8Bom(copy);
			for (const auto& [tag, block]: extractNamedBlocks(slurpFile(file.path()), 0))
			{
				if (world.getCountries().contains(tag))
					continue; // the conversion has its own definition for this tag
				if (keptTags.contains(tag) || block.find("is_historic") != std::string::npos)
				{
					copy << block << "\n\n";
					continue;
				}
				// Special tags (pirates, mercenaries, the dummy country) carry only colours and are
				// not countries the bookmark expects to find on the map.
				if (block.find("culture_definition") == std::string::npos)
				{
					copy << block << "\n\n";
					continue;
				}
				auto historic = block;
				historic.insert(historic.rfind('}'), "\tis_historic = yes\n");
				copy << historic << "\n\n";
				++historicCount;
			}
			copy.close();
		}

	std::ofstream output(folder / "zzz_converted_countries.txt");
	writeUtf8Bom(output);
	for (const auto& [tag, country]: world.getCountries())
	{
		output << tag << " = {\n";
		writeColor(output, country.color, country.coa ? country.coa->getColor2() : std::nullopt);
		output << "\tculture_definition = " << country.culture << "\n";
		output << "\treligion_definition = " << country.religion << "\n";
		output << "}\n\n";
	}
	output.close();
	Log(LogLevel::Info) << "<> " << world.getCountries().size() << " converted country definitions written; " << historicCount
							  << " vanilla countries whose land converted away were marked historic.";
}

// Custom/reformed CK3 faiths become real EU5 religions, defined in the mod's common/religions.
void writeGeneratedReligions(const std::filesystem::path& modFolder, const EU5::World& world)
{
	if (world.getGeneratedReligions().empty())
		return;
	const auto folder = modFolder / "in_game" / "common" / "religions";
	std::filesystem::create_directories(folder);
	std::ofstream output(folder / "zzz_converted_religions.txt");
	writeUtf8Bom(output);
	for (const auto& [name, religion]: world.getGeneratedReligions())
	{
		output << name << " = {\n";
		if (religion.color)
		{
			const auto [r, g, b] = religion.color->getRgbComponents();
			output << "\tcolor = rgb { " << r << " " << g << " " << b << " }\n";
		}
		else
			output << "\tcolor = rgb { 128 128 128 }\n";
		if (!religion.group.empty())
			output << "\tgroup = " << religion.group << "\n";
		if (!religion.language.empty())
			output << "\tlanguage = " << religion.language << "\n";
		// EU5 warns on religions with no modifiers and empty desc loc. A small literacy
		// bonus matches the generic "organized faith" stub vanilla uses for minor rites.
		output << "\tdefinition_modifier = {\n";
		output << "\t\tglobal_max_literacy = 5\n";
		output << "\t}\n";
		output << "}\n\n";
	}
	output.close();
	Log(LogLevel::Info) << "<> " << world.getGeneratedReligions().size() << " religions generated from CK3 faiths.";
}

// Dynamic (hybrid/divergent) CK3 cultures become real EU5 cultures, defined in the mod's common/cultures.
void writeGeneratedCultures(const std::filesystem::path& modFolder, const EU5::World& world)
{
	if (world.getGeneratedCultures().empty())
		return;
	const auto folder = modFolder / "in_game" / "common" / "cultures";
	std::filesystem::create_directories(folder);
	std::ofstream output(folder / "zzz_converted_cultures.txt");
	writeUtf8Bom(output);
	for (const auto& [name, culture]: world.getGeneratedCultures())
	{
		output << name << " = {\n";
		output << "\tlanguage = " << culture.language << "\n";
		// Vanilla gives every culture a color and the map mode expects one. A save should always
		// supply it; if one somehow doesn't, the name settles a stable color rather than none.
		if (culture.color)
		{
			const auto [r, g, b] = culture.color->getRgbComponents();
			output << "\tcolor = rgb { " << r << " " << g << " " << b << " }\n";
		}
		else
		{
			const auto seed = std::hash<std::string>{}(name);
			output << "\tcolor = rgb { " << (seed % 200 + 40) << " " << (seed / 200 % 200 + 40) << " " << (seed / 40000 % 200 + 40) << " }\n";
		}
		if (!culture.gfxTags.empty())
		{
			output << "\ttags = {";
			for (const auto& tag: culture.gfxTags)
				output << " " << tag;
			output << " }\n";
		}
		output << "\tculture_groups = {";
		for (const auto& group: culture.groups)
			output << " " << group;
		output << " }\n";
		output << "}\n\n";
	}
	output.close();
	Log(LogLevel::Info) << "<> " << world.getGeneratedCultures().size() << " cultures generated from dynamic CK3 cultures.";
}

void writeCharacter(std::ofstream& output, const EU5::ConvertedCharacter& character, const std::set<std::string>& writtenCharacters)
{
	output << "\t" << character.key << " = {\n";
	output << "\t\tfirst_name = { name = " << character.nameKey << " }\n";
	output << "\t\tculture = " << character.culture << "\n";
	output << "\t\treligion = " << character.religion << "\n";
	output << "\t\tadm = " << character.adm << " dip = " << character.dip << " mil = " << character.mil << "\n";
	if (!character.estate.empty())
		output << "\t\testate = " << character.estate << "\n";
	for (const auto& trait: character.rulerTraits)
		output << "\t\truler_trait = " << trait << "\n";
	if (!character.generalTrait.empty())
		output << "\t\tgeneral_trait = " << character.generalTrait << "\n";
	if (character.female)
		output << "\t\tfemale = yes\n";
	output << "\t\tbirth_date = " << character.birthDate << "\n";
	if (character.deathDate)
		output << "\t\tdeath_date = " << *character.deathDate << "\n";
	output << "\t\tbirth = " << character.birthLocation << "\n";
	if (!character.dynastyKey.empty())
		output << "\t\tdynasty = " << character.dynastyKey << "\n";
	// Family references must only ever point backwards to characters the game has already read.
	if (!character.fatherKey.empty() && writtenCharacters.contains(character.fatherKey))
		output << "\t\tfather = " << character.fatherKey << "\n";
	if (!character.motherKey.empty() && writtenCharacters.contains(character.motherKey))
		output << "\t\tmother = " << character.motherKey << "\n";
	if (!character.spouseKey.empty() && writtenCharacters.contains(character.spouseKey))
		output << "\t\tspouse = " << character.spouseKey << "\n";
	if (!character.tag.empty())
		output << "\t\ttag = " << character.tag << "\n";
	output << "\t}\n\n";
}

void writeLocationList(std::ofstream& output, const std::string& key, const std::vector<std::string>& locations)
{
	output << "\t\t" << key << " = {\n\t\t\t";
	auto counter = 0;
	for (const auto& location: locations)
	{
		output << location << " ";
		if (++counter % 10 == 0)
			output << "\n\t\t\t";
	}
	output << "\n\t\t}\n\n";
}

void writeStartFiles(const std::filesystem::path& startFolder,
	 const EU5::World& world,
	 const std::filesystem::path& vanillaStartFolder,
	 const configuration::Configuration& theConfiguration)
{
	// The CK3 world replaces every vanilla country on its own map, but the rest of the globe -
	// American, unconverted African/Asian natives - keeps its vanilla setup, like other converters do.
	std::set<std::string> convertedLocations;
	std::map<std::string, std::string> convertedOwners; // location -> converted tag
	for (const auto& [tag, country]: world.getCountries())
	{
		convertedLocations.insert(country.locations.begin(), country.locations.end());
		for (const auto& location: country.locations)
			convertedOwners[location] = tag;
	}

	std::set<std::string> keptTags;
	std::vector<std::pair<std::string, std::string>> keptCountries;
	auto trimmedCountries = 0;
	std::map<std::string, std::string> vanillaCapitals; // every vanilla tag -> its capital, for preference inheritance
	const auto vanillaCountriesText = slurpFile(vanillaStartFolder / "10_countries.txt");
	if (const auto outer = findBlockBody(vanillaCountriesText, "countries"); outer != std::string::npos)
	{
		if (const auto inner = findBlockBody(vanillaCountriesText, "countries", outer); inner != std::string::npos)
		{
			static const std::regex capitalRef(R"(capital\s*=\s*(\w+))");
			for (const auto& [tag, block]: extractNamedBlocks(vanillaCountriesText, inner))
			{
				if (std::smatch capital; std::regex_search(block, capital, capitalRef))
					vanillaCapitals[tag] = capital[1].str();
				if (world.getCountries().contains(tag)) // tag reused by a converted country.
					continue;
				if (!touchesConvertedLand(block, convertedLocations, world.getLocationDefinitions()))
				{
					keptTags.insert(tag);
					keptCountries.emplace_back(tag, block);
					continue;
				}
				// Partly overrun by the CK3 map: keep what the conversion left standing.
				if (const auto trimmed = trimToSurvivingLand(block, convertedLocations, world.getLocationDefinitions()))
				{
					keptTags.insert(tag);
					keptCountries.emplace_back(tag, *trimmed);
					++trimmedCountries;
				}
			}
		}
	}
	Log(LogLevel::Info) << "<> " << keptCountries.size() << " vanilla countries outside the CK3 map preserved, " << trimmedCountries
							  << " of them trimmed back to the land the conversion left them.";

	// 04 - dynasties
	std::ofstream dynasties(startFolder / "04_dynasties.txt");
	dynasties << "dynasty_manager = {\n";
	dynasties << "\tdefault_dynasty = { name = { name = default_dynasty } home = vattnajokull }\n\n";
	for (const auto& [key, dynasty]: world.getDynasties())
	{
		dynasties << "\t" << key << " = {\n";
		dynasties << "\t\tname = { name = " << key << " }\n";
		dynasties << "\t\thome = " << dynasty.home << "\n";
		dynasties << "\t}\n";
	}
	// Vanilla dynasties ride along so preserved vanilla characters keep their houses.
	const auto vanillaDynastiesText = slurpFile(vanillaStartFolder / "04_dynasties.txt");
	if (const auto body = findBlockBody(vanillaDynastiesText, "dynasty_manager"); body != std::string::npos)
		for (const auto& [key, block]: extractNamedBlocks(vanillaDynastiesText, body))
			if (key != "default_dynasty")
				dynasties << "\t" << block << "\n";
	dynasties << "}\n";
	dynasties.close();

	// 05 - characters. Every country brings its ruler's family tree, already ordered parents-first.
	std::ofstream characters(startFolder / "05_characters.txt");
	characters << "character_db={\n\n";
	std::set<std::string> writtenCharacters;
	auto characterCount = 0;
	for (const auto& [tag, country]: world.getCountries())
	{
		for (const auto& member: country.family)
		{
			if (writtenCharacters.contains(member.key))
				continue;
			writeCharacter(characters, member, writtenCharacters);
			writtenCharacters.insert(member.key);
			++characterCount;
		}
		// The court comes after the family, so any courtier who married into it can still reference
		// a spouse the game has already read.
		for (const auto& courtier: country.courtiers)
		{
			if (writtenCharacters.contains(courtier.key))
				continue;
			writeCharacter(characters, courtier, writtenCharacters);
			writtenCharacters.insert(courtier.key);
			++characterCount;
		}
	}
	// Preserved vanilla countries need their vanilla rulers and relatives back.
	std::set<std::string> forcedCharacters;
	static const std::regex governmentCharRef(R"((?:ruler|consort|heir)\s*=\s*(\w+))");
	for (const auto& [tag, block]: keptCountries)
		for (auto match = std::sregex_iterator(block.begin(), block.end(), governmentCharRef); match != std::sregex_iterator(); ++match)
			forcedCharacters.insert((*match)[1].str());
	auto vanillaCharacterCount = 0;
	const auto vanillaCharactersText = slurpFile(vanillaStartFolder / "05_characters.txt");
	if (const auto body = findBlockBody(vanillaCharactersText, "character_db"); body != std::string::npos)
	{
		static const std::regex tagRef(R"(\btag\s*=\s*(\w+))");
		// Whitespace prefix is required so character keys that merely end in "mother" don't match.
		static const std::regex familyRef(R"([ \t]+(?:father|mother|spouse)[ \t]*=[ \t]*([\w'.]+)[ \t]*\r?\n)");
		// First pass: decide which characters stay, so refs can be validated against the final set.
		const auto blocks = extractNamedBlocks(vanillaCharactersText, body);
		std::set<std::string> keptCharKeys;
		for (const auto& [key, block]: blocks)
		{
			std::smatch tagMatch;
			if ((std::regex_search(block, tagMatch, tagRef) && keptTags.contains(tagMatch[1].str())) || forcedCharacters.contains(key))
				keptCharKeys.insert(key);
		}
		// Second pass: emit kept blocks, dropping family refs to characters that fell away with converted lands.
		for (const auto& [key, block]: blocks)
		{
			if (!keptCharKeys.contains(key))
				continue;
			std::string cleaned;
			size_t last = 0;
			for (auto match = std::sregex_iterator(block.begin(), block.end(), familyRef); match != std::sregex_iterator(); ++match)
			{
				if (keptCharKeys.contains((*match)[1].str()))
					continue;
				cleaned += block.substr(last, static_cast<size_t>(match->position(0)) - last);
				last = static_cast<size_t>(match->position(0) + match->length(0));
			}
			cleaned += block.substr(last);
			characters << "\t" << cleaned << "\n";
			++vanillaCharacterCount;
		}
	}
	characters << "}\n";
	characters.close();
	Log(LogLevel::Info) << "<> " << characterCount << " characters written across " << world.getDynasties().size() << " dynasties, plus "
							  << vanillaCharacterCount << " vanilla characters preserved.";

	// The CK3 map is fully visible to every realm, so all converted countries share the same known world:
	// every region of every continent the conversion touches, plus the charted Atlantic/Indian ocean waters.
	// Without this, freshly generated tags know nothing and the world renders as terra incognita.
	std::set<std::string> knownContinents;
	for (const auto& [tag, country]: world.getCountries())
		for (const auto& location: country.locations)
			if (const auto continent = world.getLocationDefinitions().getContinentForLocation(location); !continent.empty())
				knownContinents.insert(continent);
	std::set<std::string> discoveredRegions;
	for (const auto& [continent, regions]: world.getLocationDefinitions().getContinentRegions())
	{
		const auto chartedOcean = continent.find("atlantic") != std::string::npos || continent.find("indian") != std::string::npos;
		if (knownContinents.contains(continent) || chartedOcean)
			discoveredRegions.insert(regions.begin(), regions.end());
	}
	Log(LogLevel::Info) << "<> Countries will know " << discoveredRegions.size() << " regions across " << knownContinents.size() << " continents.";

	// 10 - countries
	std::ofstream countries(startFolder / "10_countries.txt");
	countries << "current_age = age_1_traditions\n\n";
	countries << "countries = {\n";
	countries << "\tcountries = {\n\n";
	for (const auto& [tag, country]: world.getCountries())
	{
		countries << "\t" << tag << " = {\n";
		// Homeland, possession and fresh conquest, as decided by EU5::World::classifyLandControl.
		std::vector<std::string> coreLocations;
		std::vector<std::string> integratedLocations;
		std::vector<std::string> conqueredLocations;
		for (const auto& location: country.locations)
		{
			if (country.conqueredLocations.contains(location))
				conqueredLocations.push_back(location);
			else if (country.integratedLocations.contains(location))
				integratedLocations.push_back(location);
			else
				coreLocations.push_back(location);
		}
		// A realm ruling nothing it calls home still governs from somewhere, so promote its seat. It
		// has to be land the realm actually owns: a capital contested away during conversion still
		// names the winner's location, and claiming it here would have two countries own it.
		if (coreLocations.empty() && !country.locations.empty())
		{
			const auto ownsCapital = std::ranges::find(country.locations, country.capital) != country.locations.end();
			const auto& seat = ownsCapital ? country.capital : country.locations.front();
			std::erase(integratedLocations, seat);
			std::erase(conqueredLocations, seat);
			coreLocations.push_back(seat);
		}
		writeLocationList(countries, "own_control_core", coreLocations);
		if (!integratedLocations.empty())
			writeLocationList(countries, "own_control_integrated", integratedLocations);
		if (!conqueredLocations.empty())
			writeLocationList(countries, "own_control_conquered", conqueredLocations);
		// CK3 claims on foreign land arrive as cores on land someone else holds - reconquest targets.
		// Never own_core: that key means "owned but not controlled" and would steal the location.
		if (!country.coreClaims.empty())
			writeLocationList(countries, "our_cores_conquered_by_others", {country.coreClaims.begin(), country.coreClaims.end()});
		countries << "\t\tdiscovered_regions = {\n\t\t\t";
		auto counter = 0;
		for (const auto& region: discoveredRegions)
		{
			countries << region << " ";
			if (++counter % 8 == 0)
				countries << "\n\t\t\t";
		}
		countries << "\n\t\t}\n\n";
		if (!country.templateInclude.empty())
			countries << "\t\tinclude = \"" << country.templateInclude << "\"\n\n";
		countries << "\t\tcapital = " << country.capital << "\n";
		countries << "\t\tcountry_rank = " << country.rank << "\n";
		countries << "\t\tstarting_technology_level = " << country.techLevel << "\n";
		if (!country.courtLanguage.empty())
			countries << "\t\tcourt_language = " << country.courtLanguage << "\n";
		if (!country.liturgicalLanguage.empty())
			countries << "\t\tliturgical_language = " << country.liturgicalLanguage << "\n";
		if (!country.religiousSchool.empty())
			countries << "\t\treligious_school = " << country.religiousSchool << "\n";
		if (country.dynasty)
			countries << "\t\tdynasty = " << country.dynasty->key << "\n";
		// Whoever else lives here. Without these the realm counts its own subjects as foreigners.
		if (!country.acceptedCultures.empty())
		{
			countries << "\t\taccepted_cultures = {";
			for (const auto& culture: country.acceptedCultures)
				countries << " " << culture;
			countries << " }\n";
		}
		if (!country.toleratedCultures.empty())
		{
			countries << "\t\ttolerated_cultures = {";
			for (const auto& culture: country.toleratedCultures)
				countries << " " << culture;
			countries << " }\n";
		}
		countries << "\t\tgovernment = {\n";
		countries << "\t\t\ttype = " << country.governmentType << "\n";
		if (!country.heirSelection.empty())
			countries << "\t\t\their_selection = " << country.heirSelection << "\n";
		for (const auto& [value, position]: country.societalValues)
			countries << "\t\t\t" << value << " = " << position << "\n";
		if (!country.parliamentType.empty())
		{
			countries << "\t\t\tparliament = {\n";
			countries << "\t\t\t\tparliament_type = " << country.parliamentType << "\n";
			countries << "\t\t\t}\n";
		}
		if (country.ruler)
			countries << "\t\t\truler = " << country.ruler->key << "\n";
		// The dynasty's record of rule. Earlier holders of the CK3 title get closed ruler_terms, so
		// the country arrives with the history it earned instead of starting from nothing.
		for (const auto& reign: country.pastReigns)
			countries << "\t\t\truler_term = { character = " << reign.characterKey << " start_date = " << reign.startDate
						 << " end_date = " << reign.endDate << " regnal_number = " << reign.regnalNumber << " }\n";
		// The reign carried over from CK3: without a ruler_term the game treats the reign as
		// starting at the bookmark and flags any ruler traits as unearned.
		if (country.ruler && country.reignStart)
			countries << "\t\t\truler_term = { character = " << country.ruler->key << " start_date = " << *country.reignStart
						 << " regnal_number = " << country.regnalNumber << " }\n";
		if (!country.regnalNames.empty())
		{
			countries << "\t\t\tregnal_numbers = {\n";
			for (const auto& [name, count]: country.regnalNames)
				countries << "\t\t\t\t" << name << " = " << count << "\n";
			countries << "\t\t\t}\n";
		}
		if (country.consort)
			countries << "\t\t\tconsort = " << country.consort->key << "\n";
		if (country.heir && (!country.ruler || country.heir->key != country.ruler->key))
			countries << "\t\t\their = " << country.heir->key << "\n";
		// A child on the throne rules through a regency until they come of age, like CK3 handles it.
		if (country.ruler && 1337 - country.ruler->birthDate.getYear() < 16)
		{
			// Prefer the ruler's mother as regent, then any living adult relative.
			std::string regent;
			for (const auto& member: country.family)
			{
				if (member.deathDate || member.key == country.ruler->key || 1337 - member.birthDate.getYear() < 16)
					continue;
				if (member.key == country.ruler->motherKey)
				{
					regent = member.key;
					break;
				}
				if (regent.empty())
					regent = member.key;
			}
			countries << "\t\t\tregency = nobles_estate_regency\n";
			if (!regent.empty())
				countries << "\t\t\tactive_regent = " << regent << "\n";
			countries << "\t\t\tstart_regency_date = 1337.4.1\n";
			countries << "\t\t\tend_regency_date = " << country.ruler->birthDate.getYear() + 16 << "." << country.ruler->birthDate.getMonth() << "."
						 << country.ruler->birthDate.getDay() << "\n";
		}
		countries << "\t\t}\n";
		// The ruler's CK3 personal gold, carried over as starting treasury.
		if (country.treasury > 0)
			countries << "\t\tcurrency_data = {\n\t\t\tgold = " << country.treasury << "\n\t\t}\n";
		countries << "\t}\n\n";
	}
	// Vanilla countries outside the converted map keep their original entries verbatim.
	for (const auto& [tag, block]: keptCountries)
		countries << "\t" << block << "\n\n";
	countries << "\t}\n";
	countries << "}\n";
	countries.close();

	// Vanilla start files that reference vanilla tags or characters get blanked, or they would dangle.
	const auto writeBlank = [&startFolder](const std::string& fileName, const std::string& content) {
		std::ofstream blank(startFolder / fileName);
		blank << content << "\n";
		blank.close();
	};
	// Keeps vanilla entries from a countries = { countries = { TAG = {...} } } style file for preserved tags.
	const auto filterTagFile = [&](const std::string& fileName) {
		std::ofstream output(startFolder / fileName);
		output << "countries = {\n\tcountries = {\n";
		const auto vanillaText = slurpFile(vanillaStartFolder / fileName);
		if (const auto outer = findBlockBody(vanillaText, "countries"); outer != std::string::npos)
			if (const auto inner = findBlockBody(vanillaText, "countries", outer); inner != std::string::npos)
				for (const auto& [tag, block]: extractNamedBlocks(vanillaText, inner))
					if (keptTags.contains(tag))
						output << "\t\t" << block << "\n";
		output << "\t}\n}\n";
		output.close();
	};

	// 12 - diplomacy: subjects and ruler alliances carry over; preserved vanilla countries keep
	// relations among themselves.
	std::ofstream diplomacy(startFolder / "12_diplomacy.txt");
	diplomacy << "diplomacy_manager = {\n";
	for (const auto& dependency: world.getDependencies())
		diplomacy << "\tdependency = { first = " << dependency.overlord << " second = " << dependency.subject << " subject_type = " << dependency.type
					 << " }\n";
	for (const auto& [first, second]: world.getAlliances())
		diplomacy << "\tscripted_mutual = { first = " << first << " second = " << second << " type = alliance }\n";
	static const std::regex diploTagRefs(R"((?:first|second)\s*=\s*(\w+))");
	const auto vanillaDiplomacyText = slurpFile(vanillaStartFolder / "12_diplomacy.txt");
	if (const auto body = findBlockBody(vanillaDiplomacyText, "diplomacy_manager"); body != std::string::npos)
	{
		for (const auto& [kind, block]: extractNamedBlocks(vanillaDiplomacyText, body))
		{
			auto allKept = true;
			auto anyTag = false;
			for (auto match = std::sregex_iterator(block.begin(), block.end(), diploTagRefs); match != std::sregex_iterator(); ++match)
			{
				anyTag = true;
				if (!keptTags.contains((*match)[1].str()))
					allKept = false;
			}
			if (anyTag && allKept)
				diplomacy << "\t" << block << "\n";
		}
	}
	diplomacy << "}\n";
	diplomacy.close();
	Log(LogLevel::Info) << "<> " << world.getDependencies().size() << " dependencies and " << world.getAlliances().size() << " alliances written.";

	// 13 - religion. The religion_manager half is about religions, not countries, so the state of the
	// Church in 1337 (indulgences, simony, the rest) carries over untouched. The cardinal seats do
	// name countries: a seat whose patron survived keeps it, and a seat on converted land passes to
	// whoever rules there now, provided they are Catholic - otherwise the College loses that chair.
	{
		std::ofstream religion(startFolder / "13_religion.txt");
		const auto vanillaText = slurpFile(vanillaStartFolder / "13_religion.txt");
		religion << "building_manager = {\n";
		auto keptSeats = 0;
		auto movedSeats = 0;
		if (const auto body = findBlockBody(vanillaText, "building_manager"); body != std::string::npos)
		{
			static const std::regex seatTag(R"(\btag\s*=\s*(\w+))");
			static const std::regex seatLocation(R"(\blocation\s*=\s*(\w+))");
			for (const auto& [kind, block]: extractNamedBlocks(vanillaText, body))
			{
				std::smatch tagMatch;
				std::smatch locationMatch;
				if (!std::regex_search(block, tagMatch, seatTag) || !std::regex_search(block, locationMatch, seatLocation))
					continue;
				if (keptTags.contains(tagMatch[1].str()))
				{
					religion << "\t" << block << "\n";
					++keptSeats;
					continue;
				}
				const auto owner = convertedOwners.find(locationMatch[1].str());
				if (owner == convertedOwners.end())
					continue;
				const auto& country = world.getCountries().at(owner->second);
				if (country.religion != "catholic")
					continue;
				religion << "\t" << std::regex_replace(block, seatTag, "tag = " + owner->second, std::regex_constants::format_first_only) << "\n";
				++movedSeats;
			}
		}
		religion << "}\n\n";
		if (const auto body = findBlockBody(vanillaText, "religion_manager"); body != std::string::npos)
		{
			const auto end = vanillaText.rfind('}');
			religion << "religion_manager = {\n" << vanillaText.substr(body, end > body ? end - body : 0) << "}\n";
		}
		else
			religion << "religion_manager = {\n}\n";
		religion.close();
		Log(LogLevel::Info) << "<> College of Cardinals: " << keptSeats << " seats kept, " << movedSeats << " passed to converted Catholic realms.";
	}

	// 15 - international organizations: the Holy Roman Empire survives as an hre IO around the
	// converted HRE country. The engine expects the IO to exist at bookmark init; without it the
	// log floods with missing-hre errors.
	std::ofstream organizations(startFolder / "15_international_organizations.txt");
	organizations << "international_organization_manager = {\n";
	if (world.getHRETag() && theConfiguration.GetHREMode() == "io")
	{
		const auto& hreTag = *world.getHRETag();
		organizations << "\tadd_international_organization = {\n";
		organizations << "\t\ttype = hre\n";
		organizations << "\t\tcreation_date = 962.2.2\n";
		organizations << "\t\tmembers = { " << hreTag;
		// Converted vassal subjects of the HRE country join as imperial princes.
		std::set<std::string> princes;
		for (const auto& dependency: world.getDependencies())
			if (dependency.overlord == hreTag)
				princes.insert(dependency.subject);
		for (const auto& prince: princes)
			organizations << " " << prince;
		organizations << " }\n";
		organizations << "\t\tleader = " << hreTag << "\n";
		organizations << "\t\temperor = { " << hreTag << " }\n";
		if (!princes.empty())
		{
			organizations << "\t\timperial_prince = {";
			for (const auto& prince: princes)
				organizations << " " << prince;
			organizations << " }\n";
		}
		// An HRE without laws has no imperial religion or election rules, and the diet cannot function.
		// Catholic is the only imperial religion available before the Reformation.
		organizations << "\t\tlaws = {\n";
		organizations << "\t\t\thre_religion_catholic\n";
		organizations << "\t\t\tonly_imperial_religion_policy\n";
		organizations << "\t\t\tprincely_bishopric_electorate_policy\n";
		organizations << "\t\t\tno_golden_bull_policy\n";
		organizations << "\t\t\tno_monetary_contribution\n";
		organizations << "\t\t}\n";
		organizations << "\t\tvariables = {\n";
		organizations << "\t\t\timperial_authority = 5\n";
		organizations << "\t\t}\n";
		organizations << "\t}\n";
		Log(LogLevel::Info) << "<> hre international organization written around " << hreTag << ".";
	}

	// The Catholic Church, led by whoever holds the Papacy. Without it Catholic countries have no
	// curia to compete in and no papacy to influence.
	if (world.getPapacyTag())
	{
		organizations << "\tadd_international_organization = {\n";
		organizations << "\t\ttype = catholic_church\n";
		organizations << "\t\tcreation_date = 33.1.1\n";
		organizations << "\t\tmembers = { " << *world.getPapacyTag() << " }\n";
		organizations << "\t\tleader = " << *world.getPapacyTag() << "\n";
		organizations << "\t\tlaws = {\n";
		organizations << "\t\t\tlimited_indulgences\n";
		organizations << "\t\t\tultramontanism\n";
		organizations << "\t\t\tunlimited_veneration\n";
		organizations << "\t\t\ttolerated_simony\n";
		organizations << "\t\t\tsociety_of_jesus_not_allowed\n";
		organizations << "\t\t\twitchcraft_not_aknowledged\n";
		organizations << "\t\t\tpriest_marriage_not_allowed\n";
		organizations << "\t\t}\n";
		organizations << "\t}\n";
		Log(LogLevel::Info) << "<> The Catholic Church is seated at " << *world.getPapacyTag() << ".";
	}

	// Autocephalous patriarchates: the eastern churches govern themselves, one communion per religion,
	// seated in the capital of its largest realm. Their laws are the christological tenets each
	// communion actually holds, which is what the religion view reads.
	{
		static const std::map<std::string, std::vector<std::string>> patriarchalTenets = {
			 {"orthodox", {"dual_nature", "high_christology", "byzantine_rites", "cooperation_between_patriarchs_tenet", "caesaropapism_tenet"}},
			 {"miaphysite",
				  {"composite_nature", "high_christology", "local_traditions_and_rites", "independent_acting_patriarchs_tenet", "independent_authorities_tenet"}},
			 {"nestorianism",
				  {"composite_nature", "high_christology", "local_traditions_and_rites", "independent_acting_patriarchs_tenet", "independent_authorities_tenet"}}};
		static const std::vector<std::string> sharedTenets = {"accept_essence_tenet",
			 "accept_person_tenet",
			 "accept_prosopon_tenet",
			 "allow_christological_debates_tenet",
			 "allow_double_sabbath_tenet"};
		std::map<std::string, std::vector<std::string>> flocks;		  // religion -> member tags
		std::map<std::string, std::pair<size_t, std::string>> seats; // religion -> (realm size, capital)
		for (const auto& [tag, country]: world.getCountries())
		{
			if (!patriarchalTenets.contains(country.religion))
				continue;
			flocks[country.religion].push_back(tag);
			auto& seat = seats[country.religion];
			if (country.locations.size() > seat.first && !country.capital.empty())
				seat = {country.locations.size(), country.capital};
		}
		for (const auto& [religion, members]: flocks)
		{
			if (seats.at(religion).second.empty())
				continue;
			organizations << "\tadd_international_organization = {\n";
			organizations << "\t\ttype = autocephalous_patriarchate\n";
			organizations << "\t\tcreation_date = 330.1.1\n";
			organizations << "\t\tmembers = {";
			for (const auto& member: members)
				organizations << " " << member;
			organizations << " }\n";
			organizations << "\t\tvariables = {\n";
			organizations << "\t\t\treligion = religion:" << religion << "\n";
			organizations << "\t\t\tseat = location:" << seats.at(religion).second << "\n";
			organizations << "\t\t}\n";
			organizations << "\t\tlaws = {\n";
			for (const auto& tenet: patriarchalTenets.at(religion))
				organizations << "\t\t\t" << tenet << "\n";
			for (const auto& tenet: sharedTenets)
				organizations << "\t\t\t" << tenet << "\n";
			organizations << "\t\t}\n";
			organizations << "\t}\n";
			Log(LogLevel::Info) << "<> " << religion << " patriarchate seated at " << seats.at(religion).second << " over " << members.size() << " realms.";
		}
	}

	// House blocs from CK3 continue as tribal confederations between the realms their houses rule.
	// Only tribes can belong to one, and a bloc down to a single tribe dissolves on its own.
	auto confederationCount = 0;
	for (const auto& [name, members]: world.getConfederations())
	{
		std::vector<std::string> tribes;
		for (const auto& member: members)
			if (const auto& country = world.getCountries().find(member);
				 country != world.getCountries().end() && country->second.governmentType == "tribe")
				tribes.push_back(member);
		if (tribes.size() < 2)
			continue;
		organizations << "\tadd_international_organization = {\n";
		organizations << "\t\ttype = tribal_confederation\n";
		organizations << "\t\tcreation_date = 1337.1.1\n";
		organizations << "\t\tmembers = {";
		for (const auto& tribe: tribes)
			organizations << " " << tribe;
		organizations << " }\n";
		organizations << "\t\tleader = " << tribes.front() << "\n";
		organizations << "\t}\n";
		++confederationCount;
	}
	if (confederationCount > 0)
		Log(LogLevel::Info) << "<> " << confederationCount << " tribal confederations carried over from CK3 house blocs.";
	organizations << "}\n";
	organizations.close();

	// 16 - wars: active CK3 wars between converted realms continue in EU5.
	std::ofstream warsFile(startFolder / "16_wars.txt");
	warsFile << "war_manager = {\n";
	for (const auto& war: world.getWars())
	{
		warsFile << "\twar = {\n";
		warsFile << "\t\twar_name = {\n";
		warsFile << "\t\t\tname = \"" << war.nameKey << "\"\n";
		warsFile << "\t\t}\n";
		warsFile << "\t\tstart_date = " << war.startDate << "\n";
		// Without a wargoal the war screen shows "No Wargoal", the engine can't score the war or
		// declare a winner, and neither side can ever peace out - a permanent levy drain.
		if (!war.goalLocation.empty())
		{
			warsFile << "\t\ttake_province = {\n";
			warsFile << "\t\t\ttype = conquer_province\n";
			warsFile << "\t\t\tcasus_belli = cb_conquer_province\n";
			warsFile << "\t\t\tlocation = " << war.goalLocation << "\n";
			warsFile << "\t\t}\n";
		}
		auto first = true;
		for (const auto& attacker: war.attackers)
		{
			warsFile << "\t\tattacker = {\n";
			warsFile << "\t\t\tcountry = " << attacker << "\n";
			warsFile << "\t\t\trequest = {\n";
			if (first)
				warsFile << "\t\t\t\treason = Instigator\n";
			else
				warsFile << "\t\t\t\tcaller = " << war.attackers.front() << "\n\t\t\t\treason = Subject\n";
			warsFile << "\t\t\t}\n";
			warsFile << "\t\t}\n";
			first = false;
		}
		first = true;
		for (const auto& defender: war.defenders)
		{
			warsFile << "\t\tdefender = {\n";
			warsFile << "\t\t\tcountry = " << defender << "\n";
			warsFile << "\t\t\trequest = {\n";
			if (first)
				warsFile << "\t\t\t\treason = Target\n";
			else
				warsFile << "\t\t\t\tcaller = " << war.defenders.front() << "\n\t\t\t\treason = Subject\n";
			warsFile << "\t\t\t}\n";
			warsFile << "\t\t}\n";
			first = false;
		}
		warsFile << "\t}\n\n";
	}
	warsFile << "}\n";
	warsFile.close();
	Log(LogLevel::Info) << "<> " << world.getWars().size() << " wars written.";

	// 18 - opinions: grudges and goodwill between vanilla countries, kept wherever both parties are
	// still on the map.
	{
		std::ofstream opinions(startFolder / "18_opinions.txt");
		opinions << "diplomacy_manager = {\n";
		auto keptOpinions = 0;
		const auto vanillaText = slurpFile(vanillaStartFolder / "18_opinions.txt");
		if (const auto body = findBlockBody(vanillaText, "diplomacy_manager"); body != std::string::npos)
		{
			for (const auto& [kind, block]: extractNamedBlocks(vanillaText, body))
			{
				auto allKept = true;
				auto anyTag = false;
				for (auto match = std::sregex_iterator(block.begin(), block.end(), diploTagRefs); match != std::sregex_iterator(); ++match)
				{
					anyTag = true;
					if (!keptTags.contains((*match)[1].str()))
						allKept = false;
				}
				if (!anyTag || !allKept)
					continue;
				opinions << "\t" << block << "\n";
				++keptOpinions;
			}
		}
		opinions << "}\n";
		opinions.close();
		Log(LogLevel::Info) << "<> " << keptOpinions << " vanilla opinions preserved between surviving countries.";
	}

	// 20 - rivals: CK3 ruler rivalries become country rivalries, both directions like vanilla does.
	std::ofstream rivals(startFolder / "20_rivals.txt");
	rivals << "diplomacy_manager = {\n";
	for (const auto& [first, second]: world.getRivalries())
	{
		rivals << "\trival = { first = " << first << " second = " << second << " }\n";
		rivals << "\trival = { first = " << second << " second = " << first << " }\n";
	}
	rivals << "}\n";
	rivals.close();
	Log(LogLevel::Info) << "<> " << world.getRivalries().size() << " rivalries written.";

	// 22 - situations name no countries, so vanilla's file carries over as-is.
	writeBlank("22_situations.txt", slurpFile(vanillaStartFolder / "22_situations.txt"));

	// 23 - colonies: exclusive colonial claims over whole provinces. A claim survives if its owner
	// does; otherwise it passes to whoever the conversion put in that owner's place.
	{
		std::ofstream colonies(startFolder / "23_colonies.txt");
		colonies << "colony_manager = {\n";
		auto keptColonies = 0;
		const auto vanillaText = slurpFile(vanillaStartFolder / "23_colonies.txt");
		if (const auto body = findBlockBody(vanillaText, "colony_manager"); body != std::string::npos)
		{
			static const std::regex colonyTag(R"(\btag\s*=\s*(\w+))");
			for (const auto& [province, block]: extractNamedBlocks(vanillaText, body))
			{
				std::smatch tagMatch;
				if (!std::regex_search(block, tagMatch, colonyTag))
					continue;
				if (!keptTags.contains(tagMatch[1].str()))
					continue;
				colonies << "\t" << block << "\n";
				++keptColonies;
			}
		}
		colonies << "}\n";
		colonies.close();
		Log(LogLevel::Info) << "<> " << keptColonies << " vanilla colonial claims preserved.";
	}

	// 25 - area preferences: converted countries inherit the colonial/exploration preferences of the
	// vanilla countries whose land they now hold (whoever owns Lisbon explores Brazil), so the age of
	// discovery still happens; preserved vanilla countries keep their own preferences.
	std::map<std::string, std::set<std::string>> inheritedPreferences; // converted tag -> preference keys
	std::vector<std::pair<std::string, std::string>> keptPreferenceBlocks;
	const auto vanillaPreferencesText = slurpFile(vanillaStartFolder / "25_area_preferences.txt");
	if (const auto outer = findBlockBody(vanillaPreferencesText, "countries"); outer != std::string::npos)
	{
		if (const auto inner = findBlockBody(vanillaPreferencesText, "countries", outer); inner != std::string::npos)
		{
			static const std::regex preferencesList(R"(area_preferences\s*=\s*\{([^}]*)\})");
			for (const auto& [tag, block]: extractNamedBlocks(vanillaPreferencesText, inner))
			{
				if (keptTags.contains(tag))
				{
					keptPreferenceBlocks.emplace_back(tag, block);
					continue;
				}
				// The vanilla owner's land converted; its capital's new owner takes over its ambitions.
				const auto capital = vanillaCapitals.find(tag);
				if (capital == vanillaCapitals.end())
					continue;
				const auto newOwner = convertedOwners.find(capital->second);
				if (newOwner == convertedOwners.end())
					continue;
				if (std::smatch list; std::regex_search(block, list, preferencesList))
				{
					std::stringstream keys(list[1].str());
					std::string key;
					while (keys >> key)
						inheritedPreferences[newOwner->second].insert(key);
				}
			}
		}
	}
	std::ofstream preferences(startFolder / "25_area_preferences.txt");
	preferences << "countries = {\n\tcountries = {\n";
	for (const auto& [tag, keys]: inheritedPreferences)
	{
		preferences << "\t\t" << tag << " = {\n";
		preferences << "\t\t\tarea_preferences = {\n\t\t\t\t";
		for (const auto& key: keys)
			preferences << key << " ";
		preferences << "\n\t\t\t}\n";
		preferences << "\t\t}\n\n";
	}
	for (const auto& [tag, block]: keptPreferenceBlocks)
		preferences << "\t\t" << block << "\n";
	preferences << "\t}\n}\n";
	preferences.close();
	Log(LogLevel::Info) << "<> " << inheritedPreferences.size() << " converted countries inherited exploration preferences.";

	// 26 - AI personalities. Vanilla assigns these by hand to the countries it knows; a converted
	// world has none, so every realm takes the personality its CK3 ruler earned.
	{
		std::ofstream personalities(startFolder / "26_ai_personalities.txt");
		personalities << "countries = {\n\tcountries = {\n";
		auto personalityCount = 0;
		for (const auto& [tag, country]: world.getCountries())
		{
			if (country.aiPersonality.empty())
				continue;
			personalities << "\t\t" << tag << " = { ai_personality = " << country.aiPersonality << " }\n";
			++personalityCount;
		}
		personalities << "\n";
		const auto vanillaText = slurpFile(vanillaStartFolder / "26_ai_personalities.txt");
		if (const auto outer = findBlockBody(vanillaText, "countries"); outer != std::string::npos)
			if (const auto inner = findBlockBody(vanillaText, "countries", outer); inner != std::string::npos)
				for (const auto& [tag, block]: extractNamedBlocks(vanillaText, inner))
					if (keptTags.contains(tag))
						personalities << "\t\t" << block << "\n";
		personalities << "\t}\n}\n";
		personalities.close();
		Log(LogLevel::Info) << "<> " << personalityCount << " converted countries given an AI personality.";
	}

	// 27 - armies: standing armies come from the men-at-arms regiments realms actually maintained
	// in CK3, scaled down to peacetime cores (~1000 men per EU5 regiment). CK3 men-at-arms are
	// wartime mobilization; only a fraction stays under arms, and a realm-size cap keeps every
	// starting army affordable - vanilla nations start with almost no standing troops. Levies are
	// not written; EU5 raises those from pops natively. Both knobs live in dev_weights.txt.
	// The army_scale option multiplies both knobs so players can start leaner or heavier than the
	// men-at-arms ledger suggests without editing dev_weights.txt.
	auto armyScale = 1.0;
	if (theConfiguration.GetArmyScale() == "small")
		armyScale = 0.5;
	else if (theConfiguration.GetArmyScale() == "large")
		armyScale = 2.0;
	const auto maaRatio = world.getDevWeights().getMaaRatio() * armyScale;
	const auto capPerLocations = std::max(1, static_cast<int>(world.getDevWeights().getRegimentCapPerLocations() / armyScale));
	std::ofstream armies(startFolder / "27_armies.txt");
	armies << "unit_manager = {\n";
	auto writtenRegiments = 0;
	auto armedCountries = 0;
	for (const auto& [tag, country]: world.getCountries())
	{
		constexpr auto menPerRegiment = 1000;
		const auto regimentCap = static_cast<size_t>(2 + static_cast<int>(country.locations.size()) / capPerLocations);
		std::vector<std::string> subUnits;
		for (const auto& [unit, men]: country.maaUnits)
		{
			// Steppe hordes can't field the generic infantry types; their foot fights mounted.
			auto unitType = unit;
			if (country.unitCategory == "horde" && (unitType == "a_footmen" || unitType == "a_archers"))
				unitType = "a_horsemen";
			const auto keptMen = static_cast<int>(men * maaRatio);
			for (auto count = (keptMen + menPerRegiment / 2) / menPerRegiment; count > 0 && subUnits.size() < regimentCap; --count)
				subUnits.push_back(unitType);
		}
		// A realm whose men-at-arms don't add up to a single professional regiment fields none, as
		// vanilla's own countries do: they raise levies from their pops when war comes, at no
		// peacetime cost. Handing every small realm a token regiment charged hundreds of budgets
		// monthly upkeep for soldiers CK3 never said they had.
		if (subUnits.empty())
			continue;
		armies << "\tarmy = {\n";
		armies << "\t\tcountry = " << tag << "\n";
		armies << "\t\tlocation = " << country.capital << "\n";
		armies << "\t\tsub_units = {\n";
		for (const auto& unit: subUnits)
			armies << "\t\t\t" << unit << " = { strength = 1.0 }\n";
		armies << "\t\t}\n";
		armies << "\t}\n";
		writtenRegiments += static_cast<int>(subUnits.size());
		++armedCountries;
	}
	Log(LogLevel::Info) << "<> " << writtenRegiments << " standing regiments written from CK3 men-at-arms, held by " << armedCountries << " of "
							  << world.getCountries().size() << " countries.";
	// Preserved vanilla countries keep their vanilla armies and navies.
	static const std::regex armyCountryRef(R"(\bcountry\s*=\s*(\w+))");
	const auto vanillaArmiesText = slurpFile(vanillaStartFolder / "27_armies.txt");
	if (const auto body = findBlockBody(vanillaArmiesText, "unit_manager"); body != std::string::npos)
		for (const auto& [kind, block]: extractNamedBlocks(vanillaArmiesText, body))
			if (std::smatch match; std::regex_search(block, match, armyCountryRef) && keptTags.contains(match[1].str()))
				armies << "\t" << block << "\n";
	armies << "}\n";
	armies.close();
}

// 02 - core: institutions anchor to the converted world - feudalism is born at the most developed
// converted monarchy capital; the rest of the file (religious school relations) copies vanilla.
void writeCore(const std::filesystem::path& startFolder, const EU5::World& world, const std::filesystem::path& vanillaStartFolder)
{
	std::string feudalismBirthplace;
	auto bestDevelopment = -1;
	for (const auto& [tag, country]: world.getCountries())
	{
		if (country.governmentType != "monarchy")
			continue;
		const auto detail = world.getLocationDetails().find(country.capital);
		const auto development = detail != world.getLocationDetails().end() ? detail->second.development : 0;
		if (development > bestDevelopment)
		{
			bestDevelopment = development;
			feudalismBirthplace = country.capital;
		}
	}
	if (feudalismBirthplace.empty())
		return; // no suitable birthplace; vanilla 02_core stays in effect.

	const auto vanillaText = slurpFile(vanillaStartFolder / "02_core.txt");
	if (vanillaText.empty())
		return;
	// Swap only feudalism's birth_place; other institutions' birthplaces are locations and stay valid.
	const std::regex feudalism(R"((feudalism\s*=\s*\{[^}]*birth_place\s*=\s*)(\w+))");
	const auto adjusted = std::regex_replace(vanillaText, feudalism, "$1" + feudalismBirthplace);
	std::ofstream core(startFolder / "02_core.txt");
	core << adjusted;
	core.close();
	Log(LogLevel::Info) << "<> Feudalism is born at " << feudalismBirthplace << ".";
}

// 06 - pops. Start files replace vanilla wholesale, so every location is re-emitted; the ones we
// converted get their pops recultured to the CK3 county's culture/faith and scaled by development.
void writePops(const std::filesystem::path& startFolder, const EU5::World& world)
{
	const auto& details = world.getLocationDetails();
	const auto& weights = world.getDevWeights();
	std::ofstream pops(startFolder / "06_pops.txt");
	pops << "locations={\n";
	auto reculturedCount = 0;
	for (const auto& [location, vanillaPopList]: world.getVanillaPops().getLocationPops())
	{
		pops << location << " = {\n";
		const auto detailItr = details.find(location);
		if (detailItr == details.end() || detailItr->second.culture.empty() || detailItr->second.religion.empty())
		{
			for (const auto& pop: vanillaPopList)
				pops << "\tdefine_pop = { type = " << pop.type << " size = " << std::format("{:.3f}", pop.size) << " culture = " << pop.culture
					  << " religion = " << pop.religion << " }\n";
		}
		else
		{
			// All pops take the county's culture and faith; same-type pops merge, sizes scale with CK3 development.
			const auto& detail = detailItr->second;
			const auto factor =
				 std::clamp(weights.getPopBaseFactor() + detail.development * weights.getPopDevFactor(), weights.getPopBaseFactor(), weights.getPopMaxFactor());
			std::vector<std::pair<std::string, double>> mergedTypes;
			for (const auto& pop: vanillaPopList)
			{
				auto merged = std::ranges::find_if(mergedTypes, [&pop](const auto& entry) {
					return entry.first == pop.type;
				});
				if (merged == mergedTypes.end())
					mergedTypes.emplace_back(pop.type, pop.size);
				else
					merged->second += pop.size;
			}
			for (const auto& [type, size]: mergedTypes)
				pops << "\tdefine_pop = { type = " << type << " size = " << std::format("{:.3f}", size * factor) << " culture = " << detail.culture
					  << " religion = " << detail.religion << " }\n";
			++reculturedCount;
		}
		pops << "}\n";
	}
	pops << "}\n";
	pops.close();
	Log(LogLevel::Info) << "<> Pops recultured in " << reculturedCount << " locations.";
}

// The building types whose placement rules constrain conversion, scanned from the game's definitions.
struct BuildingTypeTraits
{
	// Requirements ask for a harbor or shoreline; can't stand in a landlocked town.
	std::set<std::string> port;
	// Foreign-investment buildings (is_foreign = yes: trade offices, banks, shoen...) represent a
	// relationship between two specific countries. Re-tagging one to the location's own country is
	// invalid (the game rejects a "foreign" building owned by the location owner), so they drop
	// when their investor didn't survive the conversion.
	std::set<std::string> foreign;
	// Buildings whose requirements name a particular country, culture, faith or capital belong to
	// whoever vanilla built them for. Re-tagged to a new owner they become impossible - a gallowglass
	// sept outside Gaeldom, England's sergeantry in Irish hands, a royal court in a country whose
	// capital is somewhere else - so they drop instead of converting. Only the requirement blocks are
	// read; a building that merely mentions culture in its modifiers is not tied to one.
	std::set<std::string> tied;
	// Estate buildings (estate = clergy_estate...): the estate maintains them, not the treasury.
	std::set<std::string> estateOwned;
	// Estimated monthly upkeep in gold the owner pays for one level of the type: the goods of its
	// building_maintenance production method priced at default_market_price. Zero for estate
	// buildings and for production buildings, which fund their own inputs. This is the yardstick
	// for comparing a converted country's building bill against vanilla's on the same land.
	std::map<std::string, double> upkeepGold;
};

double upkeepOf(const BuildingTypeTraits& types, const std::string& buildingType)
{
	const auto match = types.upkeepGold.find(buildingType);
	return match != types.upkeepGold.end() ? match->second : 0.0;
}

// Prices the goods of one production-method body at base market prices, or nullopt when the block
// is not a maintenance method. Goods lines look like "stone = 0.1"; anything not a known good
// (category, no_upkeep...) is skipped.
std::optional<double> priceMaintenanceMethod(const std::string& body, const std::map<std::string, double>& goodsPrices)
{
	if (body.find("building_maintenance") == std::string::npos)
		return std::nullopt;
	auto cost = 0.0;
	static const std::regex goodsLine(R"((\w+)\s*=\s*([\d.]+))");
	for (auto match = std::sregex_iterator(body.begin(), body.end(), goodsLine); match != std::sregex_iterator(); ++match)
		if (const auto price = goodsPrices.find((*match)[1].str()); price != goodsPrices.end())
			cost += std::stod((*match)[2].str()) * price->second;
	return cost;
}

BuildingTypeTraits scanBuildingTypes(const std::filesystem::path& eu5GameFolder)
{
	BuildingTypeTraits traits;
	const auto buildingTypesFolder = eu5GameFolder / "in_game" / "common" / "building_types";
	if (!std::filesystem::exists(buildingTypesFolder))
		return traits;

	// Base goods prices, for estimating what a building's monthly maintenance goods cost.
	std::map<std::string, double> goodsPrices;
	static const std::regex priceLine(R"(default_market_price\s*=\s*([\d.]+))");
	const auto goodsFolder = eu5GameFolder / "in_game" / "common" / "goods";
	if (std::filesystem::exists(goodsFolder))
		for (const auto& file: std::filesystem::directory_iterator(goodsFolder))
		{
			if (file.path().extension() != ".txt")
				continue;
			for (const auto& [goodName, block]: extractNamedBlocks(slurpFile(file.path()), 0))
				if (std::smatch match; std::regex_search(block, match, priceLine))
					goodsPrices[goodName] = std::stod(match[1].str());
		}

	// Shared maintenance methods (in_game/common/production_methods) referenced by name from
	// building types via possible_production_methods.
	std::map<std::string, double> sharedMethodCosts;
	const auto methodsFolder = eu5GameFolder / "in_game" / "common" / "production_methods";
	if (std::filesystem::exists(methodsFolder))
		for (const auto& file: std::filesystem::directory_iterator(methodsFolder))
		{
			if (file.path().extension() != ".txt")
				continue;
			for (const auto& [methodName, block]: extractNamedBlocks(slurpFile(file.path()), 0))
				if (const auto cost = priceMaintenanceMethod(block, goodsPrices); cost)
					sharedMethodCosts[methodName] = *cost;
		}

	for (const auto& file: std::filesystem::directory_iterator(buildingTypesFolder))
	{
		if (file.path().extension() != ".txt")
			continue;
		const auto typesText = slurpFile(file.path());
		for (const auto& [typeName, block]: extractNamedBlocks(typesText, 0))
		{
			if (block.find("is_port = yes") != std::string::npos || block.find("is_coastal = yes") != std::string::npos)
				traits.port.insert(typeName);
			if (block.find("is_foreign = yes") != std::string::npos)
				traits.foreign.insert(typeName);
			const auto requirements = extractBlockBody(block, "country_potential") + extractBlockBody(block, "location_potential") +
											  extractBlockBody(block, "allow") + extractBlockBody(block, "potential");
			static const std::regex ownerBound(R"(\b(tag|culture|has_culture_group|religion|is_capital|dominant_culture|has_dynasty)\s*\??=)");
			if (std::regex_search(requirements, ownerBound))
				traits.tied.insert(typeName);

			// \b keeps forbidden_for_estates from matching; underscores count as word characters.
			static const std::regex estateField(R"(\bestate\s*=\s*\w+)");
			if (std::regex_search(block, estateField))
			{
				traits.estateOwned.insert(typeName);
				continue; // the estate pays; crown upkeep is zero
			}
			// A type's own maintenance method first, then any shared one it references.
			auto upkeep = 0.0;
			for (const auto& [methodName, methodBlock]: extractNamedBlocks(extractBlockBody(block, "unique_production_methods"), 0))
				if (const auto cost = priceMaintenanceMethod(methodBlock, goodsPrices); cost)
				{
					upkeep = *cost;
					break;
				}
			if (upkeep == 0.0)
			{
				std::stringstream possible(extractBlockBody(block, "possible_production_methods"));
				std::string methodName;
				while (possible >> methodName)
					if (const auto shared = sharedMethodCosts.find(methodName); shared != sharedMethodCosts.end())
					{
						upkeep = shared->second;
						break;
					}
			}
			if (upkeep > 0.0)
				traits.upkeepGold[typeName] = upkeep;
		}
	}
	return traits;
}

// Where towns get founded and who owns which converted location.
struct TownPlan
{
	std::map<std::string, std::string> newTownSetups;	// founded location -> regional setup family
	std::set<std::string> fullCitySetupLocations;		// founded cities rich enough for a full city setup
	std::map<std::string, std::string> locationOwners; // converted location -> owning tag
};

// A founded town copies the setup most common among the vanilla towns nearest to it: the smallest
// map group (province, then area, region...) containing both it and any vanilla town decides. This
// keeps setups regional in blob empires - a realm spanning Iberia and the Maghreb founds Maghrebi
// towns with Maghrebi setups, not the empire-wide favorite. The globally most common setup is the
// last resort for towns sharing no group with any vanilla town.
//
// Founded cities are also tiered here: EU5's establishment ramp is disabled, so a full city setup
// spawns some twenty guilds at full size whether or not pops can staff them. Only the top band of
// founded cities by CK3 development carries a city setup; the rest start from the regional town
// setup and grow into more.
TownPlan planTownFoundations(const EU5::World& world)
{
	const auto& vanillaTowns = world.getVanillaTowns().getTowns();
	std::map<std::string, int> globalSetupCounts;
	for (const auto& [location, entry]: vanillaTowns)
		if (!entry.setup.empty())
			++globalSetupCounts[entry.setup];
	std::string globalSetup = "scandinavian_town";
	auto globalBest = 0;
	for (const auto& [setup, count]: globalSetupCounts)
		if (count > globalBest)
		{
			globalBest = count;
			globalSetup = setup;
		}

	TownPlan plan;
	const auto& details = world.getLocationDetails();
	std::vector<std::string> foundedLocations;
	for (const auto& [tag, country]: world.getCountries())
		for (const auto& location: country.locations)
		{
			plan.locationOwners[location] = tag;
			const auto detailItr = details.find(location);
			if (detailItr != details.end() && detailItr->second.town && !vanillaTowns.contains(location))
				foundedLocations.push_back(location);
		}

	// Map groups sorted smallest first, so the setup search widens from province to area to region.
	std::vector<const std::set<std::string>*> groupsBySize;
	for (const auto& members: world.getLocationDefinitions().getGroupLocations() | std::views::values)
		groupsBySize.push_back(&members);
	std::ranges::sort(groupsBySize, [](const auto* a, const auto* b) {
		return a->size() < b->size();
	});
	for (const auto& location: foundedLocations)
	{
		auto chosen = globalSetup;
		for (const auto* group: groupsBySize)
		{
			if (!group->contains(location))
				continue;
			std::map<std::string, int> tally;
			for (const auto& member: *group)
				if (const auto& town = vanillaTowns.find(member); town != vanillaTowns.end() && !town->second.setup.empty())
					++tally[town->second.setup];
			if (tally.empty())
				continue;
			auto best = 0;
			for (const auto& [setup, count]: tally)
				if (count > best)
				{
					best = count;
					chosen = setup;
				}
			break;
		}
		plan.newTownSetups[location] = chosen;
	}

	// The top city_setup_dev_band share of founded cities, by CK3 development and holding
	// buildings, gets a full city setup.
	std::vector<std::pair<double, std::string>> cityScores;
	for (const auto& location: foundedLocations)
		if (const auto& detail = details.at(location); detail.city)
			cityScores.emplace_back(detail.development + detail.buildings * world.getDevWeights().getBuildingWeight(), location);
	std::ranges::sort(cityScores, [](const auto& a, const auto& b) {
		return a.first != b.first ? a.first > b.first : a.second < b.second;
	});
	const auto fullSetupCount = static_cast<size_t>(std::ceil(static_cast<double>(cityScores.size()) * world.getDevWeights().getCitySetupDevBand()));
	for (size_t position = 0; position < cityScores.size() && position < fullSetupCount; ++position)
		plan.fullCitySetupLocations.insert(cityScores[position].second);
	return plan;
}

// Town setups implicitly grant each town its starting buildings, forts included. Vanilla's
// regional fort pattern is part of the game's economic balance - which towns raise castles and
// which make do with stockades is calibrated against the fort limit and the treasury - so towns
// on converted land keep their vanilla setups verbatim. What still needs adjusting is geography:
// a founded town inherits a regional setup that may be a coastal one, and handing its wharf and
// docks to a landlocked town gives the game a building it can never satisfy, so inland founded
// towns get "conv_inland_" copies stripped of port buildings.
class SetupSanitizer
{
  public:
	SetupSanitizer(const std::filesystem::path& eu5GameFolder, const std::set<std::string>& portBuildingTypes)
	{
		const auto setupsFolder = eu5GameFolder / "in_game" / "common" / "town_setups";
		if (!std::filesystem::exists(setupsFolder))
			return;
		for (const auto& file: std::filesystem::directory_iterator(setupsFolder))
		{
			if (file.path().extension() != ".txt")
				continue;
			const auto setupsText = slurpFile(file.path());
			static const std::regex buildingLine(R"((\w+)\s*=\s*(\d+))");
			for (const auto& [setupName, block]: extractNamedBlocks(setupsText, 0))
			{
				std::string landlocked;
				auto hasPorts = false;
				for (auto it = std::sregex_iterator(block.begin(), block.end(), buildingLine); it != std::sregex_iterator(); ++it)
				{
					const auto& type = (*it)[1].str();
					contents[setupName].emplace_back(type, std::stoi((*it)[2].str()));
					if (portBuildingTypes.contains(type))
						hasPorts = true;
					else
						landlocked += "\t" + type + " = " + (*it)[2].str() + "\n";
				}
				if (hasPorts)
					landlockedSetups[setupName] = landlocked;
			}
		}
	}

	// For a founded town, its regional setup - or a port-free copy when it has no harbor.
	std::string foundedSetupFor(const std::string& setup, const bool coastal)
	{
		if (coastal || !landlockedSetups.contains(setup))
			return setup;
		usedLandlockedSetups.insert(setup);
		return "conv_inland_" + setup;
	}

	// The city-grade sibling of a regional town setup (iberian_town_port -> iberian_city_port),
	// when the game defines one.
	[[nodiscard]] std::string cityVariantOf(const std::string& setup) const
	{
		const auto pos = setup.find("_town");
		if (pos == std::string::npos)
			return setup;
		auto variant = setup;
		variant.replace(pos, 5, "_city");
		return contents.contains(variant) ? variant : setup;
	}

	void writeUsedSetups(const std::filesystem::path& modFolder, const bool basicSetupUsed) const
	{
		if (usedLandlockedSetups.empty() && !basicSetupUsed)
			return;
		const auto setupsOutFolder = modFolder / "in_game" / "common" / "town_setups";
		std::filesystem::create_directories(setupsOutFolder);
		std::ofstream setupsOut(setupsOutFolder / "zzz_converted_town_setups.txt");
		writeUtf8Bom(setupsOut);
		if (basicSetupUsed)
			setupsOut << "conv_basic_town = {\n\tmarketplace = 1\n\ttemple = 1\n\tgranary = 1\n}\n\n";
		for (const auto& setup: usedLandlockedSetups)
			setupsOut << "conv_inland_" << setup << " = {\n" << landlockedSetups.at(setup) << "}\n\n";
		setupsOut.close();
	}

	// The setup's building levels, for estimating what it costs its owner every month.
	[[nodiscard]] const auto& getContents() const { return contents; }
	[[nodiscard]] auto landlockedSetupCount() const { return usedLandlockedSetups.size(); }

  private:
	std::map<std::string, std::vector<std::pair<std::string, int>>> contents; // setup name -> building type and level
	std::map<std::string, std::string> landlockedSetups;							  // setup name -> port-free body
	std::set<std::string> usedLandlockedSetups;
};

// The locations block: vanilla towns keep their rank (upgrading to city where CK3 grew one) and
// their vanilla setup. Founded towns tier by CK3 development - the top band of founded cities
// gets the full regional city setup, the rest start from the regional town setup, and ordinary
// towns start with the essentials and grow into more. Anything richer floods the market with
// unprofitable guilds no pops can work. Returns the city upgrade count; the setups written per
// location go to writtenSetups for the upkeep estimate.
int writeTownEntries(std::ofstream& cities,
	 const EU5::World& world,
	 const TownPlan& plan,
	 SetupSanitizer& setups,
	 bool& basicSetupUsed,
	 std::vector<std::pair<std::string, std::string>>& writtenSetups)
{
	const auto& details = world.getLocationDetails();
	auto upgradedCount = 0;
	for (const auto& [location, entry]: world.getVanillaTowns().getTowns())
	{
		auto rank = entry.rank;
		if (const auto detailItr = details.find(location); detailItr != details.end() && detailItr->second.city && rank == "town")
		{
			rank = "city";
			++upgradedCount;
		}
		cities << "\t" << location << " = { rank = " << rank;
		// Town setups spawn starting buildings. An unowned location (CK3-map hole whose
		// vanilla owner was trimmed away) would get those buildings with a null owner.
		if (!entry.setup.empty() && plan.locationOwners.contains(location))
		{
			cities << " town_setup = " << entry.setup;
			writtenSetups.emplace_back(location, entry.setup);
		}
		cities << " }\n";
	}
	for (const auto& [location, setup]: plan.newTownSetups)
	{
		const auto& detail = details.at(location);
		std::string chosenSetup;
		if (detail.city)
		{
			const auto family = plan.fullCitySetupLocations.contains(location) ? setups.cityVariantOf(setup) : setup;
			chosenSetup = setups.foundedSetupFor(family, !world.getLocationDefinitions().getPortSeaZone(location).empty());
		}
		else
		{
			chosenSetup = "conv_basic_town";
			basicSetupUsed = true;
		}
		cities << "\t" << location << " = { rank = " << (detail.city ? "city" : "town") << " town_setup = " << chosenSetup << " }\n";
		writtenSetups.emplace_back(location, chosenSetup);
	}
	return upgradedCount;
}

struct BuildingCounts
{
	int kept = 0;
	int retagged = 0;
	int droppedTied = 0;
	int keptForeign = 0;
	// Vanilla building_manager entries per converted owner's land, counted before any keep/drop
	// decision: the density vanilla thought that land could pay for, and thus the budget yardstick
	// for CK3 additions.
	std::map<std::string, int> vanillaOnConvertedLand;
	// Estimated monthly upkeep of those same vanilla entries, and of the entries actually written
	// for each converted tag, for the affordability comparison.
	std::map<std::string, double> vanillaUpkeepByOwner;
	std::map<std::string, double> modUpkeepByOwner;
};

// Vanilla building entries keep their tag when the owner survived, get re-tagged when the land
// converted to a new country, and drop when neither applies. Forts transfer like everything else:
// vanilla placed them where the game's fort-limit budget affords them, and deleting them only
// makes room to blanket the map with CK3's walls instead.
BuildingCounts writeVanillaBuildings(std::ofstream& cities,
	 const std::filesystem::path& vanillaStartFolder,
	 const std::set<std::string>& keptTags,
	 const std::set<std::string>& livingTags,
	 const TownPlan& plan,
	 const BuildingTypeTraits& types,
	 std::set<std::string>& writtenBuildingSlots)
{
	BuildingCounts counts;
	const auto vanillaText = slurpFile(vanillaStartFolder / "07_cities_and_buildings.txt");
	const auto body = findBlockBody(vanillaText, "building_manager");
	if (body == std::string::npos)
		return counts;
	static const std::regex tagRef(R"(\btag\s*=\s*(\w+))");
	static const std::regex locationRef(R"(\blocation\s*=\s*(\w+))");
	for (const auto& [buildingType, block]: extractNamedBlocks(vanillaText, body))
	{
		std::smatch tagMatch;
		std::smatch locationMatch;
		const auto hasTag = std::regex_search(block, tagMatch, tagRef);
		const auto hasLocation = std::regex_search(block, locationMatch, locationRef);
		const auto owner = hasLocation ? plan.locationOwners.find(locationMatch[1].str()) : plan.locationOwners.end();
		if (owner != plan.locationOwners.end())
		{
			++counts.vanillaOnConvertedLand[owner->second];
			counts.vanillaUpkeepByOwner[owner->second] += upkeepOf(types, buildingType);
		}
		if (hasTag && keptTags.contains(tagMatch[1].str()))
		{
			if (hasLocation)
				writtenBuildingSlots.insert(buildingType + "|" + locationMatch[1].str());
			cities << "\t" << block << "\n";
			++counts.kept;
			continue;
		}
		if (!hasTag || !hasLocation || owner == plan.locationOwners.end())
			continue;
		// Foreign-investment buildings represent a relationship between two countries: they keep
		// their original owner when that tag survived as a converted country, and disappear with
		// it otherwise. Re-tagging one to the location's own country is invalid.
		if (types.foreign.contains(buildingType))
		{
			if (livingTags.contains(tagMatch[1].str()) && writtenBuildingSlots.insert(buildingType + "|" + locationMatch[1].str()).second)
			{
				cities << "\t" << block << "\n";
				++counts.keptForeign;
				counts.modUpkeepByOwner[tagMatch[1].str()] += upkeepOf(types, buildingType);
			}
			continue;
		}
		if (types.tied.contains(buildingType))
		{
			++counts.droppedTied;
			continue;
		}
		if (!writtenBuildingSlots.insert(buildingType + "|" + locationMatch[1].str()).second)
			continue;
		auto retagged = block;
		retagged = std::regex_replace(retagged, tagRef, "tag = " + owner->second, std::regex_constants::format_first_only);
		cities << "\t" << retagged << "\n";
		++counts.retagged;
		counts.modUpkeepByOwner[owner->second] += upkeepOf(types, buildingType);
	}
	return counts;
}

// CK3 holdings add their own buildings on top of vanilla's. Explicit entries here are
// government-owned (tag = country) and the state pays their input goods each month, so only
// types vanilla state-owns are allowed through - the pop-owned economy comes from town setups.
//
// Additions are capped by a per-country budget scaled from how many building_manager entries
// vanilla itself placed on that same land: a realm supports the crown-building density its land
// supported in 1337, no matter how many walls and temples CK3 stacked on it. Within the budget,
// the capital comes first, then the most developed locations, so what survives the cut is the
// realm's landmarks rather than its border-fort sprawl.
int writeHoldingBuildings(std::ofstream& cities,
	 const EU5::World& world,
	 const TownPlan& plan,
	 const BuildingTypeTraits& types,
	 const std::map<std::string, int>& vanillaDensity,
	 std::set<std::string>& writtenBuildingSlots,
	 std::map<std::string, double>& modUpkeepByOwner)
{
	static const std::set<std::string> stateOwnableTypes = {"castle", "cathedral", "university"};
	// Universities only exist in towns and cities (no rural_settlement flag on the building type);
	// castles place anywhere, like vanilla's own rural fort entries.
	std::set<std::string> rankedLocations;
	std::set<std::string> cityRankLocations;
	for (const auto& [location, entry]: world.getVanillaTowns().getTowns())
	{
		rankedLocations.insert(location);
		if (entry.rank == "city" || entry.rank == "megalopolis")
			cityRankLocations.insert(location);
	}
	for (const auto& [location, setup]: plan.newTownSetups)
		rankedLocations.insert(location);
	const auto& details = world.getLocationDetails();
	for (const auto& [location, detail]: details)
		if (detail.city)
			cityRankLocations.insert(location);

	struct Candidate
	{
		bool capital = false;
		double score = 0.0;
		std::string location;
		std::string type;
	};
	std::map<std::string, std::vector<Candidate>> candidatesByTag;
	std::set<std::string> rejectedTypes;
	auto ruralUniversities = 0;
	auto nonCoreUniversities = 0;
	auto ruralCathedrals = 0;
	const auto& countries = world.getCountries();
	const auto buildingWeight = world.getDevWeights().getBuildingWeight();
	for (const auto& [location, detail]: details)
	{
		const auto owner = plan.locationOwners.find(location);
		if (owner == plan.locationOwners.end())
			continue;
		const auto countryItr = countries.find(owner->second);
		for (const auto& building: detail.eu5Buildings)
		{
			if (!stateOwnableTypes.contains(building))
			{
				rejectedTypes.insert(building);
				continue;
			}
			if (building == "university")
			{
				if (!rankedLocations.contains(location))
				{
					++ruralUniversities;
					continue;
				}
				// The building type's allow block wants the location core; land converted as
				// integrated or conquered territory is not core yet.
				if (countryItr != countries.end() &&
					 (countryItr->second.integratedLocations.contains(location) || countryItr->second.conqueredLocations.contains(location)))
				{
					++nonCoreUniversities;
					continue;
				}
			}
			// Vanilla only places explicit cathedrals under city-rank locations; a CK3 holy site
			// in a rural county has no congregation to pay for one.
			if (building == "cathedral" && !cityRankLocations.contains(location))
			{
				++ruralCathedrals;
				continue;
			}
			const auto capital = countryItr != countries.end() && countryItr->second.capital == location;
			candidatesByTag[owner->second].push_back({capital, detail.development + detail.buildings * buildingWeight, location, building});
		}
	}

	const auto allowance = world.getDevWeights().getCrownBuildingAllowance();
	auto addedBuildings = 0;
	auto droppedOverBudget = 0;
	for (auto& [tag, candidates]: candidatesByTag)
	{
		std::ranges::sort(candidates, [](const Candidate& a, const Candidate& b) {
			if (a.capital != b.capital)
				return a.capital;
			if (a.score != b.score)
				return a.score > b.score;
			if (a.location != b.location)
				return a.location < b.location;
			return a.type < b.type;
		});
		const auto vanillaItr = vanillaDensity.find(tag);
		const auto baseline = std::max(1, vanillaItr != vanillaDensity.end() ? vanillaItr->second : 0);
		auto budget = std::max(1, static_cast<int>(std::lround(baseline * allowance)));
		for (const auto& candidate: candidates)
		{
			if (budget <= 0)
			{
				++droppedOverBudget;
				continue;
			}
			if (!writtenBuildingSlots.insert(candidate.type + "|" + candidate.location).second)
				continue;
			cities << "\t" << candidate.type << " = { tag = " << tag << " level = 1 location = " << candidate.location << " }\n";
			++addedBuildings;
			--budget;
			modUpkeepByOwner[tag] += upkeepOf(types, candidate.type);
		}
	}

	if (ruralUniversities > 0)
		Log(LogLevel::Info) << "<> " << ruralUniversities << " CK3 universities sat in counties too undeveloped for an EU5 town and were not converted.";
	if (nonCoreUniversities > 0)
		Log(LogLevel::Info) << "<> " << nonCoreUniversities << " CK3 universities stood on freshly integrated or conquered land and were not converted.";
	if (ruralCathedrals > 0)
		Log(LogLevel::Info) << "<> " << ruralCathedrals << " CK3 holy sites lay outside city-rank locations and did not become cathedrals.";
	if (droppedOverBudget > 0)
		Log(LogLevel::Info) << "<> " << droppedOverBudget << " CK3 buildings exceeded their countries' crown-building budgets and were not converted.";
	for (const auto& rejected: rejectedTypes)
		Log(LogLevel::Warning) << "building_map.txt maps to " << rejected
									  << ", which vanilla never state-owns; skipped. Pop-owned buildings belong in town setups.";
	return addedBuildings;
}

// Affordability check, run at conversion time instead of discovered at the bankruptcy screen:
// for every converted country, the estimated monthly gold its written buildings bill the crown
// (explicit building_manager entries plus what its town setups spawn) is compared against what
// the vanilla buildings on that same land would have billed. Estimates use base market prices,
// identically on both sides, so the ratio is meaningful even if the absolute numbers drift.
void logUpkeepEstimates(const EU5::World& world,
	 const TownPlan& plan,
	 const BuildingTypeTraits& types,
	 const SetupSanitizer& setups,
	 const std::vector<std::pair<std::string, std::string>>& writtenSetups,
	 const BuildingCounts& counts)
{
	const auto& setupContents = setups.getContents();
	auto setupUpkeep = [&](std::string setupName) -> double {
		if (setupName == "conv_basic_town")
			return upkeepOf(types, "marketplace") + upkeepOf(types, "temple") + upkeepOf(types, "granary");
		if (setupName.starts_with("conv_inland_"))
			setupName = setupName.substr(12);
		const auto match = setupContents.find(setupName);
		if (match == setupContents.end())
			return 0.0;
		auto cost = 0.0;
		for (const auto& [type, level]: match->second)
			cost += upkeepOf(types, type) * level;
		return cost;
	};

	auto modUpkeep = counts.modUpkeepByOwner;
	auto vanillaUpkeep = counts.vanillaUpkeepByOwner;
	for (const auto& [location, setupName]: writtenSetups)
		if (const auto owner = plan.locationOwners.find(location); owner != plan.locationOwners.end())
			modUpkeep[owner->second] += setupUpkeep(setupName);
	for (const auto& [location, entry]: world.getVanillaTowns().getTowns())
		if (const auto owner = plan.locationOwners.find(location); owner != plan.locationOwners.end() && !entry.setup.empty())
			vanillaUpkeep[owner->second] += setupUpkeep(entry.setup);

	auto flagged = 0;
	auto worstRatio = 0.0;
	std::string worstTag;
	for (const auto& [tag, spend]: modUpkeep)
	{
		if (!world.getCountries().contains(tag))
			continue; // kept vanilla countries pay vanilla bills; only converted ones need auditing
		const auto baselineItr = vanillaUpkeep.find(tag);
		const auto baseline = baselineItr != vanillaUpkeep.end() ? baselineItr->second : 0.0;
		// A gold of absolute grace keeps countries on land vanilla left empty from tripping the
		// ratio over a single basic town.
		if (spend <= baseline * 1.2 + 1.0)
			continue;
		++flagged;
		const auto ratio = spend / std::max(baseline, 0.1);
		if (ratio > worstRatio)
		{
			worstRatio = ratio;
			worstTag = tag;
		}
		if (flagged <= 10)
			Log(LogLevel::Warning) << "Country " << tag << " estimated building upkeep " << std::format("{:.1f}", spend)
										  << " gold/month vs " << std::format("{:.1f}", baseline) << " for vanilla's buildings on the same land.";
	}
	if (flagged > 0)
		Log(LogLevel::Info) << "<> Building upkeep estimate: " << flagged << " of " << world.getCountries().size()
								  << " converted countries exceed 1.2x vanilla's bill on their land; worst is " << worstTag << " at "
								  << std::format("{:.1f}", worstRatio) << "x.";
	else
		Log(LogLevel::Info) << "<> Building upkeep estimate: every converted country's bill is within 1.2x of vanilla's on the same land.";
}

// 07 - cities and buildings. Vanilla town/city ranks are kept along with their setups; CK3 city
// holdings add new towns using the setup of the nearest vanilla towns. Vanilla buildings on
// converted land are re-tagged to their new owners; CK3 holy sites, wonders and universities add
// state-owned EU5 buildings under a per-country budget scaled from vanilla's own density there.
void writeCitiesAndBuildings(const std::filesystem::path& startFolder,
	 const std::filesystem::path& modFolder,
	 const EU5::World& world,
	 const std::filesystem::path& vanillaStartFolder,
	 const std::filesystem::path& eu5GameFolder,
	 const std::set<std::string>& keptTags)
{
	const auto types = scanBuildingTypes(eu5GameFolder);
	const auto plan = planTownFoundations(world);
	SetupSanitizer setups(eu5GameFolder, types.port);

	std::ofstream cities(startFolder / "07_cities_and_buildings.txt");
	cities << "locations={\n";
	auto basicSetupUsed = false;
	std::vector<std::pair<std::string, std::string>> writtenSetups; // location -> setup written, for the upkeep estimate
	const auto upgradedCount = writeTownEntries(cities, world, plan, setups, basicSetupUsed, writtenSetups);
	cities << "}\n\n";

	setups.writeUsedSetups(modFolder, basicSetupUsed);

	// Tags whose buildings can stand: surviving vanilla countries plus every converted one.
	auto livingTags = keptTags;
	for (const auto& tag: world.getCountries() | std::views::keys)
		livingTags.insert(tag);

	cities << "building_manager = {\n";
	std::set<std::string> writtenBuildingSlots; // "type|location" - the game rejects duplicates
	auto counts = writeVanillaBuildings(cities, vanillaStartFolder, keptTags, livingTags, plan, types, writtenBuildingSlots);
	const auto addedBuildings = writeHoldingBuildings(cities, world, plan, types, counts.vanillaOnConvertedLand, writtenBuildingSlots, counts.modUpkeepByOwner);
	cities << "}\n";
	cities.close();

	if (counts.droppedTied > 0)
		Log(LogLevel::Info) << "<> " << counts.droppedTied << " vanilla buildings were bound to their original owner's country, culture or capital and dropped.";
	Log(LogLevel::Info) << "<> " << plan.newTownSetups.size() << " towns founded (" << plan.fullCitySetupLocations.size() << " with full city setups), "
							  << upgradedCount << " towns upgraded to cities, " << counts.kept << " vanilla buildings kept, " << counts.retagged << " re-tagged, "
							  << counts.keptForeign << " foreign investments kept, " << addedBuildings << " added from CK3 holdings, "
							  << setups.landlockedSetupCount() << " town setups derived for landlocked towns.";

	logUpkeepEstimates(world, plan, types, setups, writtenSetups, counts);
}

// 24 - town rights. Vanilla entries stay; converted towns adopt the rights most common among
// their country's vanilla towns, keeping the flavor regional.
void writeTownRights(const std::filesystem::path& startFolder, const EU5::World& world, const std::filesystem::path& vanillaStartFolder)
{
	std::map<std::string, std::string> vanillaRights; // location -> rights key
	const auto vanillaText = slurpFile(vanillaStartFolder / "24_town_rights.txt");
	static const std::regex entry(R"((\w+)\s*=\s*(\w+))");
	if (const auto body = findBlockBody(vanillaText, "townrights_manager"); body != std::string::npos)
	{
		const auto bodyText = vanillaText.substr(body);
		for (auto match = std::sregex_iterator(bodyText.begin(), bodyText.end(), entry); match != std::sregex_iterator(); ++match)
			vanillaRights[(*match)[1].str()] = (*match)[2].str();
	}

	std::ofstream rights(startFolder / "24_town_rights.txt");
	rights << "townrights_manager = {\n";
	for (const auto& [location, rightsKey]: vanillaRights)
		rights << "\t" << location << " = " << rightsKey << "\n";
	auto granted = 0;
	const auto& details = world.getLocationDetails();
	for (const auto& [tag, country]: world.getCountries())
	{
		// The most common vanilla rights among this country's towns become its converted towns' rights.
		std::map<std::string, int> rightsCounts;
		for (const auto& location: country.locations)
			if (const auto& match = vanillaRights.find(location); match != vanillaRights.end())
				++rightsCounts[match->second];
		std::string countryRights;
		auto best = 0;
		for (const auto& [key, count]: rightsCounts)
			if (count > best)
			{
				best = count;
				countryRights = key;
			}
		if (countryRights.empty())
			continue;
		for (const auto& location: country.locations)
		{
			const auto detail = details.find(location);
			if (detail == details.end() || !detail->second.town || vanillaRights.contains(location))
				continue;
			rights << "\t" << location << " = " << countryRights << "\n";
			++granted;
		}
	}
	rights << "}\n";
	rights.close();
	if (granted > 0)
		Log(LogLevel::Info) << "<> " << granted << " converted towns received town rights.";
}

// 14 - development. The vanilla formula file is preserved and converted counties append
// per-location bonuses, so CK3 development differences show up on the EU5 map.
void writeDevelopment(const std::filesystem::path& startFolder,
	 const EU5::World& world,
	 const std::filesystem::path& vanillaStartFolder,
	 bool devImport)
{
	std::ifstream vanillaFile(vanillaStartFolder / "14_development.txt");
	std::stringstream buffer;
	buffer << vanillaFile.rdbuf();
	vanillaFile.close();
	auto content = buffer.str();
	if (const auto lastBrace = content.find_last_of('}'); lastBrace != std::string::npos)
		content = content.substr(0, lastBrace);

	std::ofstream development(startFolder / "14_development.txt");
	development << content;
	auto entryCount = 0;
	if (devImport)
	{
		const auto& weights = world.getDevWeights();
		development << "\n\t# Converted CK3 county development\n";
		for (const auto& [location, detail]: world.getLocationDetails())
		{
			const auto bonus = std::clamp(
				 static_cast<int>(detail.development / weights.getDevDivisor() + detail.buildings * weights.getBuildingWeight()),
				 0,
				 weights.getMaxBonus());
			if (bonus <= 0)
				continue;
			development << "\t" << location << " = " << bonus << "\n";
			++entryCount;
		}
	}
	development << "}\n";
	development.close();
	Log(LogLevel::Info) << "<> Development bonuses written for " << entryCount << " locations.";
}

// Finds coat of arms textures, preferring EU5's own set and copying CK3's textures into the mod when EU5 lacks them.
class CoaTextureResolver
{
  public:
	CoaTextureResolver(const std::filesystem::path& eu5Path, const std::filesystem::path& ck3Path, const std::filesystem::path& modFolder):
		 eu5Gfx(eu5Path / "game" / "main_menu" / "gfx" / "coat_of_arms"), modGfx(modFolder / "main_menu" / "gfx" / "coat_of_arms")
	{
		if (std::filesystem::exists(ck3Path / "game"))
			ck3Gfx = ck3Path / "game" / "gfx" / "coat_of_arms";
		else
			ck3Gfx = ck3Path / "gfx" / "coat_of_arms";
	}

	[[nodiscard]] std::optional<std::string> resolve(const std::string& subFolder, const std::string& texture)
	{
		if (texture.empty())
			return std::nullopt;
		if (std::filesystem::exists(eu5Gfx / subFolder / texture))
			return texture;
		if (std::filesystem::exists(modGfx / subFolder / texture))
			return texture; // already copied earlier.
		if (std::filesystem::exists(ck3Gfx / subFolder / texture))
		{
			std::filesystem::create_directories(modGfx / subFolder);
			std::filesystem::copy_file(ck3Gfx / subFolder / texture, modGfx / subFolder / texture);
			++copiedTextures;
			return texture;
		}
		return std::nullopt;
	}

	[[nodiscard]] auto getCopiedCount() const { return copiedTextures; }

  private:
	std::filesystem::path eu5Gfx;
	std::filesystem::path ck3Gfx;
	std::filesystem::path modGfx;
	int copiedTextures = 0;
};

// Translates CK3 colors back into EU5's own named palette where possible, so converted flags
// render with the same hues as native EU5 flags instead of CK3's darker literal values.
class CoaColorResolver
{
  public:
	explicit CoaColorResolver(const std::filesystem::path& eu5Path)
	{
		std::set<std::string> eu5Names;
		commonItems::parser namesParser;
		namesParser.registerRegex(commonItems::catchallRegex, [&eu5Names](const std::string& name, std::istream& theStream) {
			commonItems::ignoreItem(name, theStream);
			eu5Names.insert(name);
		});
		commonItems::parser outerParser;
		outerParser.registerKeyword("colors", [&namesParser](std::istream& theStream) {
			namesParser.parseStream(theStream);
		});
		outerParser.registerRegex(commonItems::catchallRegex, commonItems::ignoreItem);
		const auto colorsFolder = eu5Path / "game" / "main_menu" / "common" / "named_colors";
		if (commonItems::DoesFolderExist(colorsFolder))
			for (const auto& file: commonItems::GetAllFilesInFolder(colorsFolder))
				outerParser.parseFile(colorsFolder / file);

		// CK3's parser resolved named colors to rgb; map those rgb values back to names EU5 recognizes.
		for (const auto& [name, color]: laFabricaDeColor.getRegisteredColors())
			if (eu5Names.contains(name))
				rgbToName.emplace(colorKey(color), name);
	}

	[[nodiscard]] std::optional<std::string> resolve(const commonItems::Color& color) const
	{
		if (const auto& match = rgbToName.find(colorKey(color)); match != rgbToName.end())
			return match->second;
		return std::nullopt;
	}

  private:
	static std::string colorKey(const commonItems::Color& color)
	{
		const auto [r, g, b] = color.getRgbComponents();
		return std::to_string(r) + " " + std::to_string(g) + " " + std::to_string(b);
	}

	std::map<std::string, std::string> rgbToName;
};

void writeCoaColor(std::ofstream& output, const std::string& indent, const std::string& key, const commonItems::Color& color, const CoaColorResolver& colors)
{
	if (const auto& name = colors.resolve(color))
	{
		output << indent << key << " = \"" << *name << "\"\n";
		return;
	}
	const auto [r, g, b] = color.getRgbComponents();
	output << indent << key << " = rgb { " << r << " " << g << " " << b << " }\n";
}

void writeEmblemInstances(std::ofstream& output, const std::string& indent, const std::vector<CK3::EmblemInstance>& instances)
{
	for (const auto& instance: instances)
	{
		output << indent << "instance = {";
		if (instance.getPosition().size() == 2)
			output << " position = { " << instance.getPosition()[0] << " " << instance.getPosition()[1] << " }";
		if (instance.getOffset().size() == 2)
			output << " offset = { " << instance.getOffset()[0] << " " << instance.getOffset()[1] << " }";
		if (instance.getScale().size() == 2)
			output << " scale = { " << instance.getScale()[0] << " " << instance.getScale()[1] << " }";
		if (instance.getRotation() != 0.0)
			output << " rotation = " << instance.getRotation();
		if (instance.getDepth() != 0.0)
			output << " depth = " << instance.getDepth();
		output << " }\n";
	}
}

void writeEmblem(std::ofstream& output,
	 const std::string& indent,
	 const CK3::Emblem& emblem,
	 const std::string& keyword,
	 const std::string& subFolder,
	 CoaTextureResolver& textures,
	 const CoaColorResolver& colors)
{
	if (!emblem.getTexture())
		return;
	const auto texture = textures.resolve(subFolder, *emblem.getTexture());
	if (!texture)
		return; // Texture exists in neither game; dropping the emblem beats a checkerboard flag.
	output << indent << keyword << " = {\n";
	output << indent << "\ttexture = \"" << *texture << "\"\n";
	if (emblem.getColor1())
		writeCoaColor(output, indent + "\t", "color1", *emblem.getColor1(), colors);
	if (emblem.getColor2())
		writeCoaColor(output, indent + "\t", "color2", *emblem.getColor2(), colors);
	if (emblem.getColor3())
		writeCoaColor(output, indent + "\t", "color3", *emblem.getColor3(), colors);
	if (!emblem.getMask().empty())
	{
		output << indent << "\tmask = {";
		for (const auto maskEntry: emblem.getMask())
			output << " " << maskEntry;
		output << " }\n";
	}
	writeEmblemInstances(output, indent + "\t", emblem.getInstances());
	output << indent << "}\n";
}

// Writes the body of a coat of arms, recursing into sub-coats (quartered designs and the like).
// Missing fields fall back to the parent definition when the save uses parent-based inheritance.
void writeCoaBody(std::ofstream& output,
	 const std::string& indent,
	 const CK3::CoatOfArms& coa,
	 const std::optional<commonItems::Color>& fallbackColor,
	 CoaTextureResolver& textures,
	 const CoaColorResolver& colors)
{
	const CK3::CoatOfArms* parent = nullptr;
	if (coa.getParent() && coa.getParent()->second)
		parent = coa.getParent()->second.get();

	auto pattern = coa.getPattern();
	if (!pattern && parent)
		pattern = parent->getPattern();
	std::optional<std::string> resolvedPattern;
	if (pattern)
		resolvedPattern = textures.resolve("patterns", *pattern);
	output << indent << "pattern = \"" << resolvedPattern.value_or("pattern_solid.dds") << "\"\n";

	auto color1 = coa.getColor1();
	if (!color1 && parent)
		color1 = parent->getColor1();
	auto color2 = coa.getColor2();
	if (!color2 && parent)
		color2 = parent->getColor2();
	auto color3 = coa.getColor3();
	if (!color3 && parent)
		color3 = parent->getColor3();

	if (color1)
		writeCoaColor(output, indent, "color1", *color1, colors);
	else if (fallbackColor)
		writeCoaColor(output, indent, "color1", *fallbackColor, colors);
	else
		output << indent << "color1 = rgb { 128 128 128 }\n";
	if (color2)
		writeCoaColor(output, indent, "color2", *color2, colors);
	else
		output << indent << "color2 = rgb { 0 0 0 }\n";
	if (color3)
		writeCoaColor(output, indent, "color3", *color3, colors);

	const auto* emblemSource = &coa;
	if (coa.getColoredEmblems().empty() && coa.getTexturedEmblems().empty() && coa.getSubs().empty() && parent)
		emblemSource = parent;
	for (const auto& emblem: emblemSource->getColoredEmblems())
		writeEmblem(output, indent, emblem, "colored_emblem", "colored_emblems", textures, colors);
	for (const auto& emblem: emblemSource->getTexturedEmblems())
		writeEmblem(output, indent, emblem, "textured_emblem", "textured_emblems", textures, colors);

	for (const auto& sub: emblemSource->getSubs())
	{
		if (!sub)
			continue;
		output << indent << "sub = {\n";
		writeCoaBody(output, indent + "\t", *sub, fallbackColor, textures, colors);
		writeEmblemInstances(output, indent + "\t", sub->getInstances());
		output << indent << "}\n";
	}
}

void writeCoatOfArms(std::ofstream& output,
	 const std::string& tag,
	 const EU5::Country& country,
	 CoaTextureResolver& textures,
	 const CoaColorResolver& colors)
{
	output << tag << " = {\n";
	if (country.coa)
		writeCoaBody(output, "\t", *country.coa, country.color, textures, colors);
	else
	{
		output << "\tpattern = \"pattern_solid.dds\"\n";
		if (country.color)
			writeCoaColor(output, "\t", "color1", *country.color, colors);
		else
			output << "\tcolor1 = rgb { 128 128 128 }\n";
		output << "\tcolor2 = rgb { 0 0 0 }\n";
	}
	output << "}\n\n";
}

void writeCoatsOfArms(const std::filesystem::path& modFolder, const EU5::World& world, const configuration::Configuration& theConfiguration)
{
	const auto folder = modFolder / "main_menu" / "common" / "coat_of_arms" / "coat_of_arms";
	std::filesystem::create_directories(folder);
	CoaTextureResolver textures(theConfiguration.GetEU5Directory(), theConfiguration.GetCK3Directory(), modFolder);
	const CoaColorResolver colors(theConfiguration.GetEU5Directory());

	std::ofstream output(folder / "zzz_converted_coas.txt");
	auto flagged = 0;
	for (const auto& [tag, country]: world.getCountries())
	{
		writeCoatOfArms(output, tag, country, textures, colors);
		if (country.coa)
			++flagged;
	}
	// Dynasty arms: EU5 resolves dynasty coats of arms by dynasty key, exactly like country tags.
	auto dynastyFlags = 0;
	for (const auto& [key, dynasty]: world.getDynasties())
	{
		if (!dynasty.coa)
			continue;
		output << key << " = {\n";
		writeCoaBody(output, "\t", *dynasty.coa, std::nullopt, textures, colors);
		output << "}\n\n";
		++dynastyFlags;
	}
	output.close();
	Log(LogLevel::Info) << "<> " << flagged << " country and " << dynastyFlags << " dynasty coats of arms exported, " << textures.getCopiedCount()
							  << " CK3 textures copied into the mod.";
}

std::string escapeLoc(const std::string& text)
{
	std::string escaped;
	for (const auto character: text)
	{
		if (character == '"')
			escaped += '\'';
		else if (character == '\n')
			escaped += "\\n"; // a raw newline would end the yml entry mid-sentence
		else if (character == '\r' || (static_cast<unsigned char>(character) < 0x20 && character != '\t'))
			continue; // control characters, including any markup we failed to strip
		else
			escaped += character;
	}
	return escaped;
}

// Turns a location key into something readable, the way EU5's own keys mostly already read:
// "nan_sarunai" becomes "Nan Sarunai".
std::string prettifyLocationKey(const std::string& location)
{
	std::string pretty;
	auto startOfWord = true;
	for (const auto c: location)
	{
		if (c == '_')
		{
			pretty += ' ';
			startOfWord = true;
			continue;
		}
		pretty += startOfWord ? static_cast<char>(std::toupper(static_cast<unsigned char>(c))) : c;
		startOfWord = false;
	}
	return pretty;
}

// Two countries sharing a display name is an error at load and a puzzle on the map. It happens both
// ways: two CK3 titles can convert under the same name (a duchy and a kingdom of Mali), and a
// converted realm can share a name with a vanilla country that survived outside the CK3 map (our
// Venice and vanilla's VEN). The bigger realm keeps the plain name; the others are qualified by
// their capital, which is both unambiguous and the way these places were actually told apart.
struct CountryNameFixes
{
	std::map<std::string, std::string> convertedRenames; // converted tag -> qualified display name
	std::map<std::string, std::string> leftoverOverrides; // unused vanilla tag -> new loc (needs replace/)
};

CountryNameFixes disambiguateCountryNames(const EU5::World& world,
	 const std::set<std::string>& keptTags,
	 const std::filesystem::path& eu5GameFolder)
{
	std::map<std::string, std::string> taken; // display name -> the tag holding it
	std::map<std::string, std::string> vanillaNamesByTag;
	static const std::regex locEntry(R"(^\s*(\w+):\s*\d*\s*\"([^\"]*)\")");
	std::ifstream vanillaNames(eu5GameFolder / "main_menu" / "localization" / "english" / "country_names_l_english.yml");
	std::string line;
	while (std::getline(vanillaNames, line))
	{
		std::smatch match;
		if (!std::regex_search(line, match, locEntry))
			continue;
		vanillaNamesByTag.emplace(match[1].str(), match[2].str());
		if (keptTags.contains(match[1].str()))
			taken.emplace(match[2].str(), match[1].str());
	}

	// Largest first, so the realm most players would call by the bare name gets to keep it.
	std::vector<std::string> bySize;
	for (const auto& tag: world.getCountries() | std::views::keys)
		bySize.push_back(tag);
	std::ranges::sort(bySize, [&world](const std::string& first, const std::string& second) {
		const auto firstSize = world.getCountries().at(first).locations.size();
		const auto secondSize = world.getCountries().at(second).locations.size();
		return firstSize != secondSize ? firstSize > secondSize : first < second;
	});

	CountryNameFixes fixes;
	for (const auto& tag: bySize)
	{
		const auto& country = world.getCountries().at(tag);
		if (country.displayName.empty() || taken.emplace(country.displayName, tag).second)
			continue;
		auto capital = country.capital;
		if (const auto& renamed = world.getLocationRenames().find(capital); renamed != world.getLocationRenames().end())
			capital = renamed->second;
		else
			capital = prettifyLocationKey(capital);
		const auto qualified = country.displayName + " (" + capital + ")";
		if (!taken.emplace(qualified, tag).second)
			continue; // two realms sharing both name and capital is beyond telling apart
		fixes.convertedRenames[tag] = qualified;
	}
	// Historic leftovers (is_historic unused vanilla tags) keep their vanilla names and
	// collide with converted realms that reused them. Override those leftover keys.
	for (const auto& [tag, name]: vanillaNamesByTag)
	{
		if (world.getCountries().contains(tag) || keptTags.contains(tag) || name.empty())
			continue;
		const auto holder = taken.find(name);
		if (holder == taken.end() || holder->second == tag)
			continue;
		fixes.leftoverOverrides[tag] = name + " (historical)";
	}
	if (!fixes.convertedRenames.empty())
		Log(LogLevel::Info) << "<> " << fixes.convertedRenames.size() << " countries shared a name with another realm and were qualified by their capital.";
	if (!fixes.leftoverOverrides.empty())
		Log(LogLevel::Info) << "<> " << fixes.leftoverOverrides.size() << " unused vanilla country names were marked historical to avoid collisions.";
	return fixes;
}

void writeLocalization(const std::filesystem::path& modFolder,
	 const EU5::World& world,
	 const std::set<std::string>& keptTags,
	 const std::filesystem::path& eu5GameFolder)
{
	// Gather every key once, then emit identical files for all of EU5's languages so non-English
	// clients don't fall back to raw keys.
	std::vector<std::pair<std::string, std::string>> entries;
	std::set<std::string> writtenKeys;
	const auto addKey = [&entries, &writtenKeys](const std::string& key, const std::string& value) {
		if (writtenKeys.contains(key))
			return;
		writtenKeys.insert(key);
		entries.emplace_back(key, value);
	};
	const auto nameFixes = disambiguateCountryNames(world, keptTags, eu5GameFolder);
	for (const auto& [tag, country]: world.getCountries())
	{
		const auto& override = nameFixes.convertedRenames.find(tag);
		addKey(tag, override != nameFixes.convertedRenames.end() ? override->second : country.displayName);
		addKey(tag + "_ADJ", country.adjective);
		for (const auto& character: country.family)
			addKey(character.nameKey, character.rawName);
		for (const auto& courtier: country.courtiers)
			addKey(courtier.nameKey, courtier.rawName);
		for (const auto& artwork: country.artworks)
		{
			addKey(artwork.key, artwork.rawName);
			addKey(artwork.key + "_desc", artwork.rawDescription);
		}
	}
	for (const auto& [key, dynasty]: world.getDynasties())
		addKey(key, dynasty.rawName);
	for (const auto& war: world.getWars())
		addKey(war.nameKey, war.rawName);
	for (const auto& [key, religion]: world.getGeneratedReligions())
	{
		addKey(key, religion.rawName);
		addKey(key + "_ADJ", religion.rawName);
		addKey(key + "_desc", religion.rawName + " is a faith carried over from the converted Crusader Kings III campaign.");
	}
	for (const auto& [key, culture]: world.getGeneratedCultures())
		addKey(key, culture.rawName);

	const std::vector<std::string> languages =
		 {"english", "braz_por", "french", "german", "japanese", "korean", "polish", "russian", "simp_chinese", "spanish", "turkish"};
	for (const auto& language: languages)
	{
		const auto folder = modFolder / "main_menu" / "localization" / language;
		std::filesystem::create_directories(folder);
		std::ofstream output(folder / ("zzz_converted_l_" + language + ".yml"));
		output << "\xEF\xBB\xBF"; // UTF-8 BOM - paradox yml files require it.
		output << "l_" << language << ":\n";
		for (const auto& [key, value]: entries)
			output << " " << key << ": \"" << escapeLoc(value) << "\"\n";
		output.close();

		// Vanilla keys (leftover country names, player-renamed locations) need a "replace"
		// localization folder - plain mod loc files can't redefine keys vanilla already owns.
		if (world.getLocationRenames().empty() && nameFixes.leftoverOverrides.empty())
			continue;
		const auto replaceFolder = folder / "replace";
		std::filesystem::create_directories(replaceFolder);
		std::ofstream renames(replaceFolder / ("zzz_converted_renames_l_" + language + ".yml"));
		renames << "\xEF\xBB\xBF";
		renames << "l_" << language << ":\n";
		for (const auto& [tag, leftoverName]: nameFixes.leftoverOverrides)
			renames << " " << tag << ": \"" << escapeLoc(leftoverName) << "\"\n";
		for (const auto& [location, customName]: world.getLocationRenames())
			renames << " " << location << ": \"" << escapeLoc(customName) << "\"\n";
		renames.close();
	}
	Log(LogLevel::Info) << "<> " << entries.size() << " localization keys written for " << languages.size() << " languages, "
							  << world.getLocationRenames().size() << " renamed locations carried over.";
}

// 11 - art. Vanilla works of art are keyed to locations (fine), but some reference vanilla artists by
// character key, and we replace the character database wholesale. Copy the file with artist refs
// stripped, then append works of art converted from CK3 artifacts.
void writeArt(const std::filesystem::path& startFolder, const EU5::World& world, const std::filesystem::path& vanillaStartFolder)
{
	std::ifstream vanillaArt(vanillaStartFolder / "11_art.txt");
	if (!vanillaArt.is_open())
	{
		Log(LogLevel::Warning) << "Could not open vanilla 11_art.txt, skipping art.";
		return;
	}
	std::stringstream buffer;
	const std::regex artistRef(R"(artist\s*=\s*\S+\s*)");
	std::string line;
	while (std::getline(vanillaArt, line))
		buffer << std::regex_replace(line, artistRef, "") << "\n";
	vanillaArt.close();
	auto content = buffer.str();
	if (const auto lastBrace = content.find_last_of('}'); lastBrace != std::string::npos)
		content = content.substr(0, lastBrace);

	std::ofstream art(startFolder / "11_art.txt");
	art << content;
	art << "\n\t# Works of art converted from CK3 artifacts\n";
	auto artworkCount = 0;
	for (const auto& [tag, country]: world.getCountries())
		for (const auto& artwork: country.artworks)
		{
			art << "\t" << artwork.artType << " = { location = " << artwork.location << " origin = " << artwork.location << " quality = " << artwork.quality
				 << " creation_date = " << artwork.creationDate << " key = " << artwork.key << " }\n";
			++artworkCount;
		}
	art << "}\n";
	art.close();
	if (artworkCount > 0)
		Log(LogLevel::Info) << "<> " << artworkCount << " CK3 artifacts placed as works of art.";
}

// Characters: family references must point at previously defined characters. Fills seenCharacters
// with every character key the file defines, for the country check to consult.
int validateCharacterReferences(const std::filesystem::path& startFolder, std::set<std::string>& seenCharacters)
{
	auto issues = 0;
	const auto charactersText = slurpFile(startFolder / "05_characters.txt");
	const auto body = findBlockBody(charactersText, "character_db");
	if (body == std::string::npos)
		return issues;
	static const std::regex familyRef(R"([ \t](?:father|mother|spouse)[ \t]*=[ \t]*([\w'.]+))");
	for (const auto& [key, block]: extractNamedBlocks(charactersText, body))
	{
		for (auto match = std::sregex_iterator(block.begin(), block.end(), familyRef); match != std::sregex_iterator(); ++match)
		{
			const auto target = (*match)[1].str();
			// Converted characters are ordered parents-first; vanilla blocks tolerate forward refs.
			if (key.starts_with("conv_") && !seenCharacters.contains(target))
			{
				Log(LogLevel::Warning) << "Validator: " << key << " references " << target << " before its definition.";
				++issues;
			}
		}
		seenCharacters.insert(key);
	}
	return issues;
}

// Countries: every government character must exist, capitals must be owned and valid, and no
// family member may carry more ruler traits than the game accepts.
int validateCountrySheet(const EU5::World& world, const std::set<std::string>& seenCharacters)
{
	auto issues = 0;
	for (const auto& [tag, country]: world.getCountries())
	{
		for (const auto* character: {&country.ruler, &country.consort, &country.heir})
			if (*character && !seenCharacters.contains((*character)->key))
			{
				Log(LogLevel::Warning) << "Validator: " << tag << " references missing character " << (*character)->key << ".";
				++issues;
			}
		if (!world.getLocationDefinitions().isValidLocation(country.capital))
		{
			Log(LogLevel::Warning) << "Validator: " << tag << " capital " << country.capital << " is not a valid location.";
			++issues;
		}
		if (std::ranges::find(country.locations, country.capital) == country.locations.end())
		{
			Log(LogLevel::Warning) << "Validator: " << tag << " does not own its capital " << country.capital << ".";
			++issues;
		}
		for (const auto& member: country.family)
			if (member.rulerTraits.size() > 2)
			{
				Log(LogLevel::Warning) << "Validator: " << member.key << " carries " << member.rulerTraits.size() << " ruler traits (max 2).";
				++issues;
			}
	}
	return issues;
}

// Diplomacy, rivals, wars and armies must only reference written tags.
int validateTagReferences(const std::filesystem::path& startFolder, const std::set<std::string>& writtenTags)
{
	auto issues = 0;
	const auto checkTagRefs = [&](const std::string& fileName, const std::regex& pattern) {
		const auto text = slurpFile(startFolder / fileName);
		for (auto match = std::sregex_iterator(text.begin(), text.end(), pattern); match != std::sregex_iterator(); ++match)
		{
			const auto tag = (*match)[1].str();
			if (!writtenTags.contains(tag))
			{
				Log(LogLevel::Warning) << "Validator: " << fileName << " references unknown tag " << tag << ".";
				++issues;
			}
		}
	};
	static const std::regex firstSecondRef(R"((?:first|second)\s*=\s*([A-Z0-9]{3})\b)");
	static const std::regex countryRef(R"(\bcountry\s*=\s*([A-Z0-9]{3})\b)");
	checkTagRefs("12_diplomacy.txt", firstSecondRef);
	checkTagRefs("20_rivals.txt", firstSecondRef);
	checkTagRefs("16_wars.txt", countryRef);
	checkTagRefs("27_armies.txt", countryRef);
	return issues;
}

// Two countries owning the same location is the one error the game cannot paper over: the second
// owner silently wins and the first ends up with holes, or no land at all. own_core counts too -
// it means owned-but-occupied, not a claim. Cores on foreign land (our_cores_conquered_by_others)
// are deliberately left out - rival claims on the same land are the whole point of them.
int validateUniqueOwnership(const std::string& countriesText, const EU5::World& world)
{
	static const std::regex ownership(R"((own_control_core|own_control_integrated|own_control_conquered|own_control_colony|own_core)\s*=\s*\{([^}]*)\})");
	std::map<std::string, std::string> claimedBy;
	auto doubleClaims = 0;
	if (const auto outer = findBlockBody(countriesText, "countries"); outer != std::string::npos)
		if (const auto inner = findBlockBody(countriesText, "countries", outer); inner != std::string::npos)
			for (const auto& [tag, block]: extractNamedBlocks(countriesText, inner))
				for (auto match = std::sregex_iterator(block.begin(), block.end(), ownership); match != std::sregex_iterator(); ++match)
				{
					std::stringstream body((*match)[2].str());
					std::string token;
					while (body >> token)
					{
						// Vanilla blocks may claim a whole area or region by name, so groups expand to
						// the locations under them. A location name always wins over a group of the same
						// name though: EU5 names a province after its main location ("qingchi" is both a
						// location and the province holding it), and expanding those would report every
						// neighbour as a rival claimant.
						std::vector<std::string> claimed{token};
						if (const auto& definitions = world.getLocationDefinitions(); !definitions.isValidLocation(token))
							if (const auto group = definitions.getGroupLocations().find(token); group != definitions.getGroupLocations().end())
								claimed.assign(group->second.begin(), group->second.end());
						for (const auto& location: claimed)
						{
							const auto [existing, fresh] = claimedBy.emplace(location, tag);
							if (!fresh && existing->second != tag)
							{
								if (doubleClaims < 10)
									Log(LogLevel::Warning) << "Validator: " << location << " is claimed by both " << existing->second << " and " << tag << ".";
								++doubleClaims;
							}
						}
					}
				}
	if (doubleClaims > 0)
		Log(LogLevel::Warning) << "Validator: " << doubleClaims << " locations are claimed by more than one country.";
	return doubleClaims;
}

// Templates, cultures and religions the country sheet names have to exist somewhere the game reads.
int validateNamedDefinitions(const EU5::World& world, const std::filesystem::path& eu5GameFolder)
{
	std::set<std::string> knownTemplates;
	const auto templateFolder = eu5GameFolder / "main_menu" / "setup" / "templates";
	if (std::filesystem::exists(templateFolder))
		for (const auto& file: std::filesystem::recursive_directory_iterator(templateFolder))
			if (file.is_regular_file() && file.path().extension() == ".txt")
				knownTemplates.insert(file.path().stem().string()); // a template is named by its file
	const auto cultureExists = [&](const std::string& culture) {
		return world.getGameDatabase().isValidCulture(culture) || world.getGeneratedCultures().contains(culture);
	};
	const auto religionExists = [&](const std::string& religion) {
		return world.getGameDatabase().isValidReligion(religion) || world.getGeneratedReligions().contains(religion);
	};

	auto badTemplates = 0;
	auto badCultures = 0;
	auto badReligions = 0;
	for (const auto& [tag, country]: world.getCountries())
	{
		if (!knownTemplates.empty() && !country.templateInclude.empty() && !knownTemplates.contains(country.templateInclude))
		{
			if (badTemplates < 5)
				Log(LogLevel::Warning) << "Validator: " << tag << " includes unknown template " << country.templateInclude << ".";
			++badTemplates;
		}
		if (!country.culture.empty() && !cultureExists(country.culture))
		{
			if (badCultures < 5)
				Log(LogLevel::Warning) << "Validator: " << tag << " has unknown primary culture " << country.culture << ".";
			++badCultures;
		}
		for (const auto& culture: country.acceptedCultures)
			if (!cultureExists(culture))
			{
				if (badCultures < 5)
					Log(LogLevel::Warning) << "Validator: " << tag << " accepts unknown culture " << culture << ".";
				++badCultures;
			}
		if (!country.religion.empty() && !religionExists(country.religion))
		{
			if (badReligions < 5)
				Log(LogLevel::Warning) << "Validator: " << tag << " has unknown religion " << country.religion << ".";
			++badReligions;
		}
	}
	return badTemplates + badCultures + badReligions;
}

// Building entries: one per type and location, and only in locations that exist.
int validateBuildingEntries(const std::filesystem::path& startFolder, const EU5::World& world)
{
	const auto buildingsText = slurpFile(startFolder / "07_cities_and_buildings.txt");
	static const std::regex buildingEntry(R"(add_building\s*=\s*\{[^}]*?type\s*=\s*(\w+)[^}]*?location\s*=\s*(\w+))");
	std::set<std::string> slots;
	auto duplicates = 0;
	auto badLocations = 0;
	for (auto match = std::sregex_iterator(buildingsText.begin(), buildingsText.end(), buildingEntry); match != std::sregex_iterator(); ++match)
	{
		const auto slot = (*match)[1].str() + "|" + (*match)[2].str();
		if (!slots.insert(slot).second)
		{
			if (duplicates < 5)
				Log(LogLevel::Warning) << "Validator: duplicate building " << slot << ".";
			++duplicates;
		}
		if (!world.getLocationDefinitions().isValidLocation((*match)[2].str()))
		{
			if (badLocations < 5)
				Log(LogLevel::Warning) << "Validator: building in unknown location " << (*match)[2].str() << ".";
			++badLocations;
		}
	}
	return duplicates + badLocations;
}

// International organizations: members and leaders have to be countries that made it into the save.
int validateOrganizationMembers(const std::filesystem::path& startFolder, const std::set<std::string>& writtenTags)
{
	const auto organizationsText = slurpFile(startFolder / "15_international_organizations.txt");
	static const std::regex memberList(R"((members|emperor)\s*=\s*\{([^}]*)\})");
	static const std::regex leaderRef(R"(leader\s*=\s*([A-Z0-9]{3})\b)");
	auto badMembers = 0;
	for (auto match = std::sregex_iterator(organizationsText.begin(), organizationsText.end(), memberList); match != std::sregex_iterator(); ++match)
	{
		std::stringstream body((*match)[2].str());
		std::string tag;
		while (body >> tag)
			if (!writtenTags.contains(tag))
			{
				Log(LogLevel::Warning) << "Validator: international organization has unknown member " << tag << ".";
				++badMembers;
			}
	}
	for (auto match = std::sregex_iterator(organizationsText.begin(), organizationsText.end(), leaderRef); match != std::sregex_iterator(); ++match)
		if (!writtenTags.contains((*match)[1].str()))
		{
			Log(LogLevel::Warning) << "Validator: international organization led by unknown tag " << (*match)[1].str() << ".";
			++badMembers;
		}
	return badMembers;
}

// Town setups: every location we rank as a town has to name a setup the mod or the game defines.
int validateTownSetupReferences(const std::filesystem::path& startFolder, const std::filesystem::path& modFolder, const std::filesystem::path& eu5GameFolder)
{
	std::set<std::string> knownSetups;
	static const std::regex setupName(R"(^([\w]+)\s*=\s*\{)");
	for (const auto& folder: {eu5GameFolder / "in_game" / "common" / "town_setups", modFolder / "in_game" / "common" / "town_setups"})
	{
		if (!std::filesystem::exists(folder))
			continue;
		for (const auto& file: std::filesystem::recursive_directory_iterator(folder))
		{
			if (!file.is_regular_file() || file.path().extension() != ".txt")
				continue;
			std::ifstream input(file.path());
			std::string line;
			while (std::getline(input, line))
			{
				// A UTF-8 BOM hides a setup defined on a file's first line from the ^ anchor.
				if (line.starts_with("\xEF\xBB\xBF"))
					line.erase(0, 3);
				if (std::smatch match; std::regex_search(line, match, setupName))
					knownSetups.insert(match[1].str());
			}
		}
	}
	const auto townRightsText = slurpFile(startFolder / "24_town_rights.txt");
	const auto buildingsText = slurpFile(startFolder / "07_cities_and_buildings.txt");
	static const std::regex setupRef(R"(town_setup\s*=\s*(\w+))");
	std::set<std::string> missingSetups;
	for (const auto* text: {&townRightsText, &buildingsText})
		for (auto match = std::sregex_iterator(text->begin(), text->end(), setupRef); match != std::sregex_iterator(); ++match)
			if (!knownSetups.empty() && !knownSetups.contains((*match)[1].str()))
				missingSetups.insert((*match)[1].str());
	for (const auto& setup: missingSetups)
		Log(LogLevel::Warning) << "Validator: unknown town setup " << setup << ".";
	return static_cast<int>(missingSetups.size());
}

// Post-write sanity scan of the generated mod. Catches regressions before the game does.
void validateOutput(const std::filesystem::path& startFolder, const std::filesystem::path& modFolder, const EU5::World& world, const std::filesystem::path& eu5GameFolder)
{
	std::set<std::string> seenCharacters;
	auto issues = validateCharacterReferences(startFolder, seenCharacters);

	const auto countriesText = slurpFile(startFolder / "10_countries.txt");
	std::set<std::string> writtenTags;
	if (const auto outer = findBlockBody(countriesText, "countries"); outer != std::string::npos)
		if (const auto inner = findBlockBody(countriesText, "countries", outer); inner != std::string::npos)
			for (const auto& [tag, block]: extractNamedBlocks(countriesText, inner))
				writtenTags.insert(tag);

	issues += validateCountrySheet(world, seenCharacters);
	issues += validateTagReferences(startFolder, writtenTags);
	issues += validateUniqueOwnership(countriesText, world);
	issues += validateNamedDefinitions(world, eu5GameFolder);
	issues += validateBuildingEntries(startFolder, world);
	issues += validateOrganizationMembers(startFolder, writtenTags);
	issues += validateTownSetupReferences(startFolder, modFolder, eu5GameFolder);

	if (issues == 0)
		Log(LogLevel::Info) << "<> Validation passed: no dangling references detected.";
	else
		Log(LogLevel::Warning) << "<> Validation found " << issues << " issues; the mod may still load, but expect oddities.";
}
} // namespace

void EU5::outputWorld(const World& world, const configuration::Configuration& theConfiguration)
{
	const auto modName = theConfiguration.GetOutputName();
	// The mod is staged here rather than installed directly: Fronter copies output/<modName> into
	// the game's mod folder and activates a launcher playset for it.
	const auto modFolder = std::filesystem::path("output") / modName;
	Log(LogLevel::Info) << "*** Writing mod to " << modFolder.string() << " ***";

	if (std::filesystem::exists(modFolder))
		std::filesystem::remove_all(modFolder);
	std::filesystem::create_directories(modFolder);

	writeMetadata(modFolder, modName);
	writeGeneratedReligions(modFolder, world);
	writeGeneratedCultures(modFolder, world);

	const auto startFolder = modFolder / "main_menu" / "setup" / "start";
	std::filesystem::create_directories(startFolder);
	const auto vanillaStartFolder = theConfiguration.GetEU5Directory() / "game" / "main_menu" / "setup" / "start";
	writeStartFiles(startFolder, world, vanillaStartFolder, theConfiguration);

	// The kept-tags set is needed again for buildings; recompute it cheaply from the generated 10_countries.
	std::set<std::string> keptTags;
	{
		const auto generated = slurpFile(startFolder / "10_countries.txt");
		if (const auto outer = findBlockBody(generated, "countries"); outer != std::string::npos)
			if (const auto inner = findBlockBody(generated, "countries", outer); inner != std::string::npos)
				for (const auto& [tag, block]: extractNamedBlocks(generated, inner))
					if (!world.getCountries().contains(tag))
						keptTags.insert(tag);
	}

	writeCountryDefinitions(modFolder, world, theConfiguration.GetEU5Directory() / "game", keptTags);
	writeCore(startFolder, world, vanillaStartFolder);
	writePops(startFolder, world);
	writeCitiesAndBuildings(startFolder, modFolder, world, vanillaStartFolder, theConfiguration.GetEU5Directory() / "game", keptTags);
	writeTownRights(startFolder, world, vanillaStartFolder);
	writeDevelopment(startFolder, world, vanillaStartFolder, theConfiguration.GetDevImport());
	writeArt(startFolder, world, vanillaStartFolder);
	writeCoatsOfArms(modFolder, world, theConfiguration);
	writeLocalization(modFolder, world, keptTags, theConfiguration.GetEU5Directory() / "game");
	validateOutput(startFolder, modFolder, world, theConfiguration.GetEU5Directory() / "game");

	Log(LogLevel::Info) << "<> Mod written to " << modFolder.string() << ".";
}
