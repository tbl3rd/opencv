/*M///////////////////////////////////////////////////////////////////////////////////////
//
//  IMPORTANT: READ BEFORE DOWNLOADING, COPYING, INSTALLING OR USING.
//
//  By downloading, copying, installing or using the software you agree to this license.
//  If you do not agree to this license, do not download, install,
//  copy or use the software.
//
//
//                           License Agreement
//                For Open Source Computer Vision Library
//
// Copyright (C) 2010-2012, Institute Of Software Chinese Academy Of Science, all rights reserved.
// Copyright (C) 2010-2012, Advanced Micro Devices, Inc., all rights reserved.
// Copyright (C) 2010-2012, Multicoreware, Inc., all rights reserved.
// Third party copyrights are property of their respective owners.
//
// @Authors
//    Niko Li, newlife20080214@gmail.com
//    Jia Haipeng, jiahaipeng95@gmail.com
//    Shengen Yan, yanshengen@gmail.com
//    Jiang Liyuan, lyuan001.good@163.com
//    Rock Li, Rock.Li@amd.com
//    Wu Zailong, bullet@yeah.net
//    Xu Pang, pangxu010@163.com
//    Sen Liu, swjtuls1987@126.com
//
// Redistribution and use in source and binary forms, with or without modification,
// are permitted provided that the following conditions are met:
//
//   * Redistribution's of source code must retain the above copyright notice,
//     this list of conditions and the following disclaimer.
//
//   * Redistribution's in binary form must reproduce the above copyright notice,
//     this list of conditions and the following disclaimer in the documentation
//     and/or other materials provided with the distribution.
//
//   * The name of the copyright holders may not be used to endorse or promote products
//     derived from this software without specific prior written permission.
//
// This software is provided by the copyright holders and contributors "as is" and
// any express or implied warranties, including, but not limited to, the implied
// warranties of merchantability and fitness for a particular purpose are disclaimed.
// In no event shall the Intel Corporation or contributors be liable for any direct,
// indirect, incidental, special, exemplary, or consequential damages
// (including, but not limited to, procurement of substitute goods or services;
// loss of use, data, or profits; or business interruption) however caused
// and on any theory of liability, whether in contract, strict liability,
// or tort (including negligence or otherwise) arising in any way out of
// the use of this software, even if advised of the possibility of such damage.
//
//M*/

#include "test_precomp.hpp"
#include "cvconfig.h"
#include "opencv2/ts/ocl_test.hpp"

#ifdef HAVE_OPENCL

namespace cvtest {
namespace ocl {

///////////////////////////////////////////////////////////////////////////////

PARAM_TEST_CASE(CalcBackProject, MatDepth, int, bool)
{
    int depth, N;
    bool useRoi;

    std::vector<float> ranges;
    std::vector<int> channels;
    double scale;

    std::vector<Mat> images;
    std::vector<Mat> images_roi;
    std::vector<UMat> uimages;
    std::vector<UMat> uimages_roi;

    TEST_DECLARE_INPUT_PARAMETER(hist)
    TEST_DECLARE_OUTPUT_PARAMETER(dst)

    virtual void SetUp()
    {
        depth = GET_PARAM(0);
        N = GET_PARAM(1);
        useRoi = GET_PARAM(2);

        ASSERT_GE(2, N);

        images.resize(N);
        images_roi.resize(N);
        uimages.resize(N);
        uimages_roi.resize(N);
    }

    virtual void random_roi()
    {
        Size roiSize = randomSize(1, MAX_VALUE);

        int totalChannels = 0;
        for (int i = 0; i < N; ++i)
        {
            Border srcBorder = randomBorder(0, useRoi ? MAX_VALUE : 0);
            int cn = randomInt(1, 5);
            randomSubMat(images[i], images_roi[i], roiSize, srcBorder, CV_MAKE_TYPE(depth, cn), 0, 125);

            ranges.push_back(10);
            ranges.push_back(100);

            channels.push_back(randomInt(0, cn) + totalChannels);
            totalChannels += cn;
        }

        Mat tmpHist;
        {
            std::vector<int> hist_size(N);
            for (int i = 0 ; i < N; ++i)
                hist_size[i] = randomInt(10, 50);

            cv::calcHist(images_roi, channels, noArray(), tmpHist, hist_size, ranges);
            ASSERT_EQ(CV_32FC1, tmpHist.type());
        }

        Border histBorder = randomBorder(0, useRoi ? MAX_VALUE : 0);
        randomSubMat(hist, hist_roi, tmpHist.size(), histBorder, tmpHist.type(), 0, MAX_VALUE);
        tmpHist.copyTo(hist_roi);

        Border dstBorder = randomBorder(0, useRoi ? MAX_VALUE : 0);
        randomSubMat(dst, dst_roi, roiSize, dstBorder, CV_MAKE_TYPE(depth, 1), 5, 16);

        for (int i = 0; i < N; ++i)
        {
            images[i].copyTo(uimages[i]);

            Size _wholeSize;
            Point ofs;
            images_roi[i].locateROI(_wholeSize, ofs);

            uimages_roi[i] = uimages[i](Rect(ofs.x, ofs.y, images_roi[i].cols, images_roi[i].rows));
        }

        UMAT_UPLOAD_INPUT_PARAMETER(hist)
        UMAT_UPLOAD_OUTPUT_PARAMETER(dst)

        scale = randomDouble(0.1, 1);
    }

    void Near()
    {
        OCL_EXPECT_MATS_NEAR(dst, 0.0)
    }
};

//////////////////////////////// CalcBackProject //////////////////////////////////////////////

OCL_TEST_P(CalcBackProject, Mat)
{
    for (int j = 0; j < test_loop_times; j++)
    {
        random_roi();

        OCL_OFF(cv::calcBackProject(images_roi, channels, hist_roi, dst_roi, ranges, scale));
        OCL_ON(cv::calcBackProject(uimages_roi, channels, uhist_roi, udst_roi, ranges, scale));

        Near();
    }
}

/////////////////////////////////////////////////////////////////////////////////////

OCL_INSTANTIATE_TEST_CASE_P(Imgproc, CalcBackProject, Combine(Values((MatDepth)CV_8U), Values(1, 2), Bool()));

} } // namespace cvtest::ocl

#endif // HAVE_OPENCL
