/**
 * @file src/cpdetect/cpdetect.cpp
 * @brief Methods of CompilerDetector class.
 * @copyright (c) 2017 Avast Software, licensed under the MIT license
 */

#include "retdec/utils/conversion.h"
#include "retdec/utils/binary_path.h"
#include "retdec/utils/equality.h"
#include "retdec/utils/filesystem.h"
#include "retdec/utils/string.h"
#include "retdec/cpdetect/cpdetect.h"
#include "retdec/cpdetect/heuristics/elf_heuristics.h"
#include "retdec/cpdetect/heuristics/heuristics.h"
#include "retdec/cpdetect/heuristics/macho_heuristics.h"
#include "retdec/cpdetect/heuristics/pe_heuristics.h"
#include "retdec/cpdetect/settings.h"
#include "retdec/yaracpp/yara_detector.h"

using namespace retdec::fileformat;
using namespace retdec::utils;
using namespace retdec::yaracpp;

namespace retdec {
namespace cpdetect {

namespace
{

/**
 * Decide better detection by version or extra information
 *
 * @param a first detection
 * @param b second detection
 * @param result @c true if first detection is better, @c false otherwise
 * @return @c true if @p result is defined, @c false otherwise
 *
 * Warning: sort function requires strict weak ordering!
 */
bool compareExtraInfo(
		const DetectResult &a,
		const DetectResult &b,
		bool &result)
{
	// Check by version
	if (!a.versionInfo.empty() && b.versionInfo.empty())
	{
		// Prefer detection with version
		result = true;
		return true;
	}
	if (a.versionInfo.empty() && !b.versionInfo.empty())
	{
		// Prefer detection with version
		result = false;
		return true;
	}

	// Check by extra info
	if (!a.additionalInfo.empty() && b.additionalInfo.empty())
	{
		// Prefer detection with extra info
		result = true;
		return true;
	}
	if (a.additionalInfo.empty() && !b.additionalInfo.empty())
	{
		// Prefer detection with extra info
		result = false;
		return true;
	}

	return false;
}

/**
 * Decide better detection
 *
 * @param a first detection
 * @param b second detection
 * @return @c true if first detection is better, @c false otherwise
 *
 * Warning: sort function requires strict weak ordering!
 */
bool compareForSort(const DetectResult &a, const DetectResult &b)
{
	if (a.strength == b.strength)
	{
		if (a.source == DetectionMethod::SIGNATURE && a.source == b.source)
		{
			// Equaly strong signature detections - check nibble counts
			const auto aRatio = static_cast<double>(a.agreeCount) / a.impCount;
			const auto bRatio = static_cast<double>(b.agreeCount) / b.impCount;
			if (areEqual(aRatio, bRatio))
			{
				if (isShorterPrefixOfCaseInsensitive(a.name, b.name)
						&& a.impCount == b.impCount)
				{
					// Decide by version or extra information
					bool compRes = false;
					return compareExtraInfo(a, b, compRes) ? compRes : false;
				}
				else
				{
					// Prefer bigger signature
					return a.impCount > b.impCount;
				}
			}
			else
			{
				// Prefer better match
				return aRatio > bRatio;
			}
		}

		// Everything is better than incomplete signature detection
		if (b.source == DetectionMethod::SIGNATURE
				&& b.agreeCount != b.impCount)
		{
			return true;
		}
		else if (a.source == DetectionMethod::SIGNATURE
				&& a.agreeCount != a.impCount)
		{
			return false;
		}

		// If both are same compilers with same detection strength
		if (isShorterPrefixOfCaseInsensitive(a.name, b.name))
		{
			// Decide by version or extra information
			bool compRes = false;
			if (compareExtraInfo(a, b, compRes))
			{
				return compRes;
			}
		}

		// Prefer heuristic
		return b.source == DetectionMethod::SIGNATURE;
	}

	// Prefer stronger method
	return a.strength > b.strength;
}

/**
 * Convert meta to type of tool.
 */
ToolType metaToTool(const std::string &toolMeta)
{
	if (toolMeta == "C")
	{
		return ToolType::COMPILER;
	}
	if (toolMeta == "P")
	{
		return ToolType::PACKER;
	}
	if (toolMeta == "L")
	{
		return ToolType::LINKER;
	}
	if (toolMeta == "I")
	{
		return ToolType::INSTALLER;
	}

	return ToolType::UNKNOWN;
}

/**
* Convert meta to type of tool.
*/
retdec::cpdetect::DetectionStrength metaToStrength(
		const retdec::yaracpp::YaraMeta * strengthMeta)
{
	if (strengthMeta)
	{
		const auto & toolStrength = strengthMeta->getStringValue();

		if (areEqualCaseInsensitive(toolStrength, "low"))
			return DetectionStrength::LOW;

		if (areEqualCaseInsensitive(toolStrength, "medium"))
			return DetectionStrength::MEDIUM;

		if (areEqualCaseInsensitive(toolStrength, "high"))
			return DetectionStrength::HIGH;

		if (areEqualCaseInsensitive(toolStrength, "sure"))
			return DetectionStrength::SURE;
	}

	return DetectionStrength::MEDIUM;
}

} // anonymous namespace

/**
 * Constructor
 *
 * Constructor in subclass must create members @a heuristics,
 * @a internalDatabase and @a externalSuffixes
 */
CompilerDetector::CompilerDetector(
		retdec::fileformat::FileFormat &parser,
		DetectParams &params,
		ToolInformation &toolInfo)
		: fileParser(parser)
		, cpParams(params)
		, toolInfo(toolInfo)
		, targetArchitecture(fileParser.getTargetArchitecture())
		, search(fileParser)
		, pathToShared(getThisBinaryDirectoryPath())
{
	externalSuffixes = EXTERNAL_DATABASE_SUFFIXES;

	bool isFat = false;
	fs::path path(pathToShared);
	path.append(YARA_RULES_PATH);
	std::set<std::string> formats;
	std::set<std::string> archs;
	switch (fileParser.getFileFormat())
	{
		case Format::ELF:
		{
			auto& elf = *static_cast<fileformat::ElfFormat*>(&fileParser);
			heuristics = std::make_unique<ElfHeuristics>(elf, search, toolInfo);
			formats.insert("elf");
			break;
		}
		case Format::PE:
		{
			auto& pe = *static_cast<fileformat::PeFormat*>(&fileParser);
			heuristics = std::make_unique<PeHeuristics>(pe, search, toolInfo);
			formats.insert("pe");
			break;
		}
		case Format::MACHO:
		{
			auto& macho = *static_cast<fileformat::MachOFormat*>(&fileParser);
			heuristics = std::make_unique<MachOHeuristics>(macho, search, toolInfo);
			formats.insert("macho");
			isFat = macho.isFatBinary();
			break;
		}
		case Format::RAW_DATA:
		{
			heuristics = std::make_unique<Heuristics>(parser, search, toolInfo);
			// Any format.
			formats.insert("elf");
			formats.insert("macho");
			formats.insert("pe");
			break;
		}
		case Format::INTEL_HEX:
		{
			heuristics = std::make_unique<Heuristics>(parser, search, toolInfo);
			// Intel Hex was most likely created from ELF.
			formats.insert("elf");
			break;
		}
		case Format::COFF:
		{
			heuristics = std::make_unique<Heuristics>(parser, search, toolInfo);
			// Maybe something from PE will hit.
			formats.insert("pe");
			break;
		}
		default:
		{
			heuristics = std::make_unique<Heuristics>(parser, search, toolInfo);
			break;
		}
	}

	auto bitWidth = fileParser.getWordLength();

	if (!isFat)
	switch(targetArchitecture)
	{
		case Architecture::X86_64:
			archs.insert("x64");
			[[fallthrough]];
		case Architecture::X86:
			archs.insert("x86");
			break;

		case Architecture::ARM:
			if (bitWidth == 32)
			{
				archs.insert("arm");
			}
			else
			{
				archs.insert("arm64");
			}
			break;

		case Architecture::MIPS:
			if (bitWidth == 32)
			{
				archs.insert("mips");
			}
			else
			{
				archs.insert("mips64");
			}
			break;

		case Architecture::POWERPC:
			if (bitWidth == 32)
			{
				archs.insert("ppc");
			}
			else
			{
				archs.insert("ppc64");
			}
			break;

		default:
			break;
	}

	populateInternalPaths(path, formats, archs);
}

/**
 * External databases parsing
 * @return @c true if at least one external database was detected,
 * @c false otherwise
 */
bool CompilerDetector::getExternalDatabases()
{
	auto result = false;

	// iterating over all files in directory
	for(auto& subpathIt: fs::directory_iterator("."))
	{
		auto subpath = subpathIt.path();
		if (fs::is_regular_file(subpath)
				&& std::any_of(externalSuffixes.begin(), externalSuffixes.end(),
			[&] (const auto &suffix)
			{
				return endsWith(subpath.string(), suffix);
			}
		))
		{
			result = true;
			externalDatabase.push_back(subpath.string());
		}
	}

	return result;
}

/**
 * Remove every detected compiler wchich has less similarity than @a refRatio
 *
 * Compilers which have not been detected based on signatures are not affected
 * @param refRatio Similarity ratio
 */
void CompilerDetector::removeCompilersWithLessSimilarity(double refRatio)
{
	double actRatio;

	for (std::size_t i = 0, e = toolInfo.detectedTools.size(); i < e; ++i)
	{
		if (toolInfo.detectedTools[i].source == DetectionMethod::SIGNATURE)
		{
			actRatio = static_cast<double>(toolInfo.detectedTools[i].agreeCount)
					/ toolInfo.detectedTools[i].impCount;
			auto ratio = actRatio + std::numeric_limits<double>::epsilon()
					 * std::abs(actRatio);
			if (ratio < refRatio)
			{
				toolInfo.detectedTools.erase(toolInfo.detectedTools.begin() + i);
				--i;
				--e;
			}
		}
	}
}

/**
 * Remove redundant compilers
 */
void CompilerDetector::removeUnusedCompilers()
{
	std::size_t noOfCompilers = toolInfo.detectedTools.size();
	std::size_t lastBeneficial = 0;
	auto removeFlag = false;

	for (std::size_t i = 0; i < noOfCompilers; ++i)
	{
		if (toolInfo.isReliableResult(i))
		{
			lastBeneficial = i;
			removeFlag = true;
		}
	}

	if (removeFlag)
	{
		for (std::size_t i = lastBeneficial + 1; i < noOfCompilers; ++i)
		{
			if (toolInfo.detectedTools[i].source < DetectionMethod::SIGNATURE)
			{
				toolInfo.detectedTools.erase(toolInfo.detectedTools.begin() + i);
				--i;
				--noOfCompilers;
			}
		}
	}

	for (std::size_t i = 0; i < noOfCompilers; ++i)
	{
		const auto &first = toolInfo.detectedTools[i];

		for (std::size_t j = i + 1; j < noOfCompilers; ++j)
		{
			const auto &second = toolInfo.detectedTools[j];
			if (first.name == second.name
					&& (first.versionInfo == second.versionInfo
						|| second.versionInfo.empty())
					&& (first.additionalInfo == second.additionalInfo
						|| second.additionalInfo.empty()))
			{
				toolInfo.detectedTools.erase(toolInfo.detectedTools.begin() + j);
				--j;
				--noOfCompilers;
			}
		}
	}
}

/**
 * Add all YARA files from the given @p dir to internal paths member.
 * Expects @p dir structure like this: formats/archs/files
 */
void CompilerDetector::populateInternalPaths(
		const fs::path& dir,
		const std::set<std::string>& formats,
		const std::set<std::string>& archs)
{
	if (!fs::is_directory(dir))
	{
		return;
	}

	for(auto& sub1It: fs::directory_iterator(dir))
	{
		auto sub1 = sub1It.path();
		if (!(fs::is_directory(sub1) && endsWith(sub1.string(), formats)))
		{
			continue;
		}

		for(auto& sub2It: fs::directory_iterator(sub1))
		{
			auto sub2 = sub2It.path();
			if (!(fs::is_directory(sub2)
					&& (archs.empty() || endsWith(sub2.string(), archs))))
			{
				continue;
			}

			for(auto& sub3It: fs::directory_iterator(sub2))
			{
				auto sub3 = sub3It.path();
				if (!(fs::is_regular_file(sub3) && endsWith(sub3.string(), externalSuffixes)))
				{
					continue;
				}

				internalPaths.push_back(sub3.string());
			}
		}
	}
}

/**
 * Try detect used compiler (or packer) based on heuristics
 */
void CompilerDetector::getAllHeuristics()
{
	if (heuristics)
	{
		heuristics->getAllHeuristics();
	}
}

/**
 * Try detect used compiler (or packer) based on signatures
 * @return Status of detection (ReturnCode::OK if all is OK)
 */
ReturnCode CompilerDetector::getAllSignatures()
{
	YaraDetector yara;

	// Add internal paths.
	unsigned iCntr = 0;
	for (const auto &ruleFile : internalPaths)
	{
		std::string nameSpace = "internal_" + std::to_string(iCntr++);
		yara.addRuleFile(ruleFile, nameSpace);
	}

	unsigned eCntr = 0;
	if (cpParams.external && getExternalDatabases())
	{
		for (const auto &item : externalDatabase)
		{
			std::string nameSpace = "external_" + std::to_string(eCntr++);
			yara.addRuleFile(item, nameSpace);
		}
	}

	yara.analyze(
			fileParser.getPathToFile(),
			cpParams.searchType != SearchType::EXACT_MATCH
	);
	const auto &detected = yara.getDetectedRules();
	const auto &undetected = yara.getUndetectedRules();
	auto result = false;
	if (cpParams.searchType == SearchType::EXACT_MATCH
			|| (cpParams.searchType == SearchType::MOST_SIMILAR
					&& !detected.empty()))
	{
		for (const auto &rule : detected)
		{
			// The rule requires to have the "name" meta-tag
			const auto *nameMeta = rule.getMeta("name");
			if (nameMeta)
			{
				const auto *toolMeta = rule.getMeta("tool");
				const auto *versionMeta = rule.getMeta("version");
				const auto *commentMeta = rule.getMeta("comment");
				const auto & version = versionMeta
						? versionMeta->getStringValue()
						: "";
				auto toolType = toolMeta
						? metaToTool(toolMeta->getStringValue())
						: ToolType::UNKNOWN;

				// Use "extra" meta-tag for extra value if comment is
				// not available
				if (!commentMeta)
					commentMeta = rule.getMeta("extra");
				const auto & extra = commentMeta
						? commentMeta->getStringValue()
						: "";

				// If the rule has the "pattern" meta-tag, it means
				// that it's a nibble detection
				const auto *patternMeta = rule.getMeta("pattern");
				if (patternMeta && rule.getFirstMatch())
				{
					const auto nibbles = search.countImpNibbles(
							patternMeta->getStringValue());
					if (nibbles)
					{
						result = true;
						const auto *languageMeta = rule.getMeta("language");
						const auto *bytecodeMeta = rule.getMeta("bytecode");
						commentMeta = commentMeta
								? commentMeta
								: rule.getMeta("extra");
						toolInfo.addTool(
								nibbles,
								nibbles,
								toolType,
								nameMeta->getStringValue(),
								version,
								extra);
						if (languageMeta)
						{
							toolInfo.addLanguage(
									languageMeta->getStringValue(),
									"",
									bytecodeMeta ? true : false);
						}
					}
				}
				// If there is no "pattern" meta-tag, we consider
				// the rule to be a heuristic detection
				else
				{
					toolInfo.addTool(
						DetectionMethod::YARA_RULE,
						metaToStrength(rule.getMeta("strength")),
						toolType,
						nameMeta->getStringValue(),
						version,
						extra
					);
				}
			}
		}

		return (result ? ReturnCode::OK : ReturnCode::UNKNOWN_CP);
	}

	Similarity sim;
	double maxRatio = 0.0;

	for (const auto &rules : {detected, undetected})
	{
		for (const auto &rule : rules)
		{
			const auto *nameMeta = rule.getMeta("name");
			auto *patternMeta = rule.getMeta("pattern");
			if (!nameMeta || !patternMeta)
			{
				continue;
			}
			auto pattern = patternMeta->getStringValue();
			while (endsWith(pattern,  ";"))
			{
				pattern.pop_back();
			}
			const auto *match = rule.getFirstMatch();
			const auto *toolMeta = rule.getMeta("tool");
			const auto *versionMeta = rule.getMeta("version");
			const auto *commentMeta = rule.getMeta("comment");
			commentMeta = commentMeta ? commentMeta : rule.getMeta("extra");
			if (match)
			{
				const auto nibbles = search.countImpNibbles(pattern);
				if (nibbles)
				{
					result = true;
					maxRatio = 1.0;
					toolInfo.addTool(
							nibbles,
							nibbles,
							toolMeta
									? metaToTool(toolMeta->getStringValue())
									: ToolType::UNKNOWN,
							nameMeta->getStringValue(),
							versionMeta ? versionMeta->getStringValue() : "",
							commentMeta ? commentMeta->getStringValue() : ""
					);
				}
				continue;
			}

			std::size_t base = 0;
			const auto *absoluteStartMeta = rule.getMeta("absoluteStart");
			if (absoluteStartMeta)
			{
				if (absoluteStartMeta->getType() == yaracpp::YaraMeta::Type::String
					&& !strToNum(absoluteStartMeta->getStringValue(), base))
				{
					continue;
				}
			}
			else if (toolInfo.entryPointOffset)
			{
				base = toolInfo.epOffset;
			}
			else
			{
				continue;
			}

			std::size_t startShift = 0, endShift = 0;
			const auto *startMeta = rule.getMeta("start");
			const auto *endMeta = rule.getMeta("end");
			if (startMeta)
			{
				startShift = startMeta->getIntValue();
			}
			if (endMeta)
			{
				endShift = endMeta->getIntValue();
			}
			const auto start = base + startShift;
			const auto end = base
					+ endShift
					+ fileParser.bytesFromNibblesRounded(pattern.length()) - 1;
			if (search.areaSimilarity(pattern, sim, start, end)
					&& (cpParams.searchType == SearchType::SIM_LIST
						|| (cpParams.searchType == SearchType::MOST_SIMILAR
							&& sim.ratio >= maxRatio)))
			{
				result = true;
				maxRatio = sim.ratio;
				toolInfo.addTool(
						sim.same,
						sim.total,
						toolMeta
								? metaToTool(toolMeta->getStringValue())
								: ToolType::UNKNOWN,
						nameMeta->getStringValue(),
						versionMeta
								? versionMeta->getStringValue()
								: "",
						commentMeta
								? commentMeta->getStringValue() :
								""
				);
			}
		}
	}

	if (cpParams.searchType == SearchType::MOST_SIMILAR)
	{
		removeCompilersWithLessSimilarity(maxRatio);
	}

	return (result ? ReturnCode::OK : ReturnCode::UNKNOWN_CP);
}

/**
 * Detects all compilers (and packers) based on signatures and heuristics
 * @return Status of detection (ReturnCode::OK if all is OK)
 */
ReturnCode CompilerDetector::getAllCompilers()
{
	const auto status = getAllSignatures();
	getAllHeuristics();
	std::stable_sort(
			toolInfo.detectedTools.begin(),
			toolInfo.detectedTools.end(),
			compareForSort);
	removeUnusedCompilers();
	if (toolInfo.detectedLanguages.empty())
	{
		for (const auto &item : toolInfo.detectedTools)
		{
			if (!item.isReliable())
			{
				continue;
			}

			const auto name = toLower(item.name);
			if (contains(name, ".net"))
			{
				toolInfo.addLanguage("CIL/.NET", "", true);
			}
		}
	}

	const bool isDetecteion = toolInfo.detectedTools.size()
			|| toolInfo.detectedLanguages.size();
	return status == ReturnCode::UNKNOWN_CP && isDetecteion
			? ReturnCode::OK
			: status;
}

/**
 * Detect all supported information about used compiler or packer
 * @return Status of detection (ReturnCode::OK if all is OK)
 *
 * Compiler can be successfully detected even if is returned a value other
 * than ReturnCode::OK
 */
ReturnCode CompilerDetector::getAllInformation()
{
	if (!fileParser.isInValidState())
	{
		return ReturnCode::FILE_PROBLEM;
	}

	fileParser.getImageBaseAddress(toolInfo.imageBase);

	toolInfo.entryPointAddress = fileParser.getEpAddress(toolInfo.epAddress);
	toolInfo.entryPointOffset = fileParser.getEpOffset(toolInfo.epOffset);

	if ((toolInfo.overlaySize = fileParser.getOverlaySize()) != 0)
	{
		toolInfo.overlayOffset = fileParser.getDeclaredFileLength();
	}

	bool invalidEntryPoint = false;
	Format format = fileParser.getFileFormat();
	if (format == Format::PE)
	{
		// False EP offset (offset outside of file) doesn't have
		// to mean invalid EP as it can be memory only
		invalidEntryPoint = !toolInfo.entryPointAddress;
	}
	else
	{
		invalidEntryPoint = !toolInfo.entryPointAddress || !toolInfo.entryPointOffset;
	}

	if (!fileParser.getHexEpBytes(toolInfo.epBytes, cpParams.epBytesCount)
			&& !invalidEntryPoint
			&& !fileParser.isInValidState())
	{
		return ReturnCode::FILE_PROBLEM;
	}

	const auto *epSec = fileParser.getEpSection();
	toolInfo.entryPointSection = epSec;
	if (epSec)
	{
		toolInfo.epSection = Section(*epSec);
	}

	auto status = getAllCompilers();
	if (invalidEntryPoint)
	{
		if (fileParser.isExecutable()
				|| toolInfo.entryPointAddress
				|| toolInfo.entryPointSection)
		{
			status = ReturnCode::ENTRY_POINT_DETECTION;
		}
	}

	return status;
}

} // namespace cpdetect
} // namespace retdec
