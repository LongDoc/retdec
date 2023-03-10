/**
 * @file src/fileinfo/file_detector/pe_detector.h
 * @brief Definition of PeDetector class.
 * @copyright (c) 2017 Avast Software, licensed under the MIT license
 */

#ifndef FILEINFO_FILE_DETECTOR_PE_DETECTOR_H
#define FILEINFO_FILE_DETECTOR_PE_DETECTOR_H

#include "fileinfo/file_detector/file_detector.h"
#include "fileinfo/file_wrapper/pe_wrapper.h"

namespace retdec {
namespace fileinfo {

/**
 * PeDetector - find info about PE binary file
 */
class PeDetector : public FileDetector
{
	private:
		std::shared_ptr<PeWrapper> peParser; ///< file parser

		/// @name Auxiliary detection methods
		/// @{
		void getFileFlags();
		void getDllFlags();
		void getHeaderInfo();
		void getCoffSymbols();
		void getRelocationTableInfo();
		void getDirectories();
		void getSections();
		void getDotnetInfo();
		void getVisualBasicInfo();
		void getTimestamps();
		/// @}
	protected:
		/// @name Detection methods
		/// @{
		virtual void detectFileClass() override;
		virtual void detectArchitecture() override;
		virtual void detectFileType() override;
		virtual void getAdditionalInfo() override;
		virtual retdec::cpdetect::CompilerDetector* createCompilerDetector() const override;
		/// @}
	public:
		PeDetector(
				const std::string & pathToInputFile,
				const std::string & dllListFile,
				FileInformation &finfo,
				retdec::cpdetect::DetectParams &searchPar,
				retdec::fileformat::LoadFlags loadFlags);
};

} // namespace fileinfo
} // namespace retdec

#endif
