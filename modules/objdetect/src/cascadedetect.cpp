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
// Copyright (C) 2008-2013, Itseez Inc., all rights reserved.
// Third party copyrights are property of their respective owners.
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
//   * The name of Itseez Inc. may not be used to endorse or promote products
//     derived from this software without specific prior written permission.
//
// This software is provided by the copyright holders and contributors "as is" and
// any express or implied warranties, including, but not limited to, the implied
// warranties of merchantability and fitness for a particular purpose are disclaimed.
// In no event shall the copyright holders or contributors be liable for any direct,
// indirect, incidental, special, exemplary, or consequential damages
// (including, but not limited to, procurement of substitute goods or services;
// loss of use, data, or profits; or business interruption) however caused
// and on any theory of liability, whether in contract, strict liability,
// or tort (including negligence or otherwise) arising in any way out of
// the use of this software, even if advised of the possibility of such damage.
//
//M*/

#include "precomp.hpp"
#include <cstdio>

#include "cascadedetect.hpp"
#include "opencv2/objdetect/objdetect_c.h"
#include "opencl_kernels.hpp"

#if defined (LOG_CASCADE_STATISTIC)
struct Logger
{
    enum { STADIES_NUM = 20 };

    int gid;
    cv::Mat mask;
    cv::Size sz0;
    int step;


    Logger() : gid (0), step(2) {}
    void setImage(const cv::Mat& image)
    {
     if (gid == 0)
         sz0 = image.size();

      mask.create(image.rows, image.cols * (STADIES_NUM + 1) + STADIES_NUM, CV_8UC1);
      mask = cv::Scalar(0);
      cv::Mat roi = mask(cv::Rect(cv::Point(0,0), image.size()));
      image.copyTo(roi);

      printf("%d) Size = (%d, %d)\n", gid, image.cols, image.rows);

      for(int i = 0; i < STADIES_NUM; ++i)
      {
          int x = image.cols + i * (image.cols + 1);
          cv::line(mask, cv::Point(x, 0), cv::Point(x, mask.rows-1), cv::Scalar(255));
      }

      if (sz0.width/image.cols > 2 && sz0.height/image.rows > 2)
          step = 1;
    }

    void setPoint(const cv::Point& p, int passed_stadies)
    {
        int cols = mask.cols / (STADIES_NUM + 1);

        passed_stadies = -passed_stadies;
        passed_stadies = (passed_stadies == -1) ? STADIES_NUM : passed_stadies;

        unsigned char* ptr = mask.ptr<unsigned char>(p.y) + cols + 1 + p.x;
        for(int i = 0; i < passed_stadies; ++i, ptr += cols + 1)
        {
            *ptr = 255;

            if (step == 2)
            {
                ptr[1] = 255;
                ptr[mask.step] = 255;
                ptr[mask.step + 1] = 255;
            }
        }
    };

    void write()
    {
        char buf[4096];
        sprintf(buf, "%04d.png", gid++);
        cv::imwrite(buf, mask);
    }

} logger;
#endif

namespace cv
{

template<typename _Tp> void copyVectorToUMat(const std::vector<_Tp>& v, UMat& um)
{
    if(v.empty())
        um.release();
    Mat(1, (int)(v.size()*sizeof(v[0])), CV_8U, (void*)&v[0]).copyTo(um);
}

void groupRectangles(std::vector<Rect>& rectList, int groupThreshold, double eps, std::vector<int>* weights, std::vector<double>* levelWeights)
{
    if( groupThreshold <= 0 || rectList.empty() )
    {
        if( weights )
        {
            size_t i, sz = rectList.size();
            weights->resize(sz);
            for( i = 0; i < sz; i++ )
                (*weights)[i] = 1;
        }
        return;
    }

    std::vector<int> labels;
    int nclasses = partition(rectList, labels, SimilarRects(eps));

    std::vector<Rect> rrects(nclasses);
    std::vector<int> rweights(nclasses, 0);
    std::vector<int> rejectLevels(nclasses, 0);
    std::vector<double> rejectWeights(nclasses, DBL_MIN);
    int i, j, nlabels = (int)labels.size();
    for( i = 0; i < nlabels; i++ )
    {
        int cls = labels[i];
        rrects[cls].x += rectList[i].x;
        rrects[cls].y += rectList[i].y;
        rrects[cls].width += rectList[i].width;
        rrects[cls].height += rectList[i].height;
        rweights[cls]++;
    }
    if ( levelWeights && weights && !weights->empty() && !levelWeights->empty() )
    {
        for( i = 0; i < nlabels; i++ )
        {
            int cls = labels[i];
            if( (*weights)[i] > rejectLevels[cls] )
            {
                rejectLevels[cls] = (*weights)[i];
                rejectWeights[cls] = (*levelWeights)[i];
            }
            else if( ( (*weights)[i] == rejectLevels[cls] ) && ( (*levelWeights)[i] > rejectWeights[cls] ) )
                rejectWeights[cls] = (*levelWeights)[i];
        }
    }

    for( i = 0; i < nclasses; i++ )
    {
        Rect r = rrects[i];
        float s = 1.f/rweights[i];
        rrects[i] = Rect(saturate_cast<int>(r.x*s),
             saturate_cast<int>(r.y*s),
             saturate_cast<int>(r.width*s),
             saturate_cast<int>(r.height*s));
    }

    rectList.clear();
    if( weights )
        weights->clear();
    if( levelWeights )
        levelWeights->clear();

    for( i = 0; i < nclasses; i++ )
    {
        Rect r1 = rrects[i];
        int n1 = rweights[i];
        double w1 = rejectWeights[i];
        int l1 = rejectLevels[i];

        // filter out rectangles which don't have enough similar rectangles
        if( n1 <= groupThreshold )
            continue;
        // filter out small face rectangles inside large rectangles
        for( j = 0; j < nclasses; j++ )
        {
            int n2 = rweights[j];

            if( j == i || n2 <= groupThreshold )
                continue;
            Rect r2 = rrects[j];

            int dx = saturate_cast<int>( r2.width * eps );
            int dy = saturate_cast<int>( r2.height * eps );

            if( i != j &&
                r1.x >= r2.x - dx &&
                r1.y >= r2.y - dy &&
                r1.x + r1.width <= r2.x + r2.width + dx &&
                r1.y + r1.height <= r2.y + r2.height + dy &&
                (n2 > std::max(3, n1) || n1 < 3) )
                break;
        }

        if( j == nclasses )
        {
            rectList.push_back(r1);
            if( weights )
                weights->push_back(l1);
            if( levelWeights )
                levelWeights->push_back(w1);
        }
    }
}

class MeanshiftGrouping
{
public:
    MeanshiftGrouping(const Point3d& densKer, const std::vector<Point3d>& posV,
        const std::vector<double>& wV, double eps, int maxIter = 20)
    {
        densityKernel = densKer;
        weightsV = wV;
        positionsV = posV;
        positionsCount = (int)posV.size();
        meanshiftV.resize(positionsCount);
        distanceV.resize(positionsCount);
        iterMax = maxIter;
        modeEps = eps;

        for (unsigned i = 0; i<positionsV.size(); i++)
        {
            meanshiftV[i] = getNewValue(positionsV[i]);
            distanceV[i] = moveToMode(meanshiftV[i]);
            meanshiftV[i] -= positionsV[i];
        }
    }

    void getModes(std::vector<Point3d>& modesV, std::vector<double>& resWeightsV, const double eps)
    {
        for (size_t i=0; i <distanceV.size(); i++)
        {
            bool is_found = false;
            for(size_t j=0; j<modesV.size(); j++)
            {
                if ( getDistance(distanceV[i], modesV[j]) < eps)
                {
                    is_found=true;
                    break;
                }
            }
            if (!is_found)
            {
                modesV.push_back(distanceV[i]);
            }
        }

        resWeightsV.resize(modesV.size());

        for (size_t i=0; i<modesV.size(); i++)
        {
            resWeightsV[i] = getResultWeight(modesV[i]);
        }
    }

protected:
    std::vector<Point3d> positionsV;
    std::vector<double> weightsV;

    Point3d densityKernel;
    int positionsCount;

    std::vector<Point3d> meanshiftV;
    std::vector<Point3d> distanceV;
    int iterMax;
    double modeEps;

    Point3d getNewValue(const Point3d& inPt) const
    {
        Point3d resPoint(.0);
        Point3d ratPoint(.0);
        for (size_t i=0; i<positionsV.size(); i++)
        {
            Point3d aPt= positionsV[i];
            Point3d bPt = inPt;
            Point3d sPt = densityKernel;

            sPt.x *= std::exp(aPt.z);
            sPt.y *= std::exp(aPt.z);

            aPt.x /= sPt.x;
            aPt.y /= sPt.y;
            aPt.z /= sPt.z;

            bPt.x /= sPt.x;
            bPt.y /= sPt.y;
            bPt.z /= sPt.z;

            double w = (weightsV[i])*std::exp(-((aPt-bPt).dot(aPt-bPt))/2)/std::sqrt(sPt.dot(Point3d(1,1,1)));

            resPoint += w*aPt;

            ratPoint.x += w/sPt.x;
            ratPoint.y += w/sPt.y;
            ratPoint.z += w/sPt.z;
        }
        resPoint.x /= ratPoint.x;
        resPoint.y /= ratPoint.y;
        resPoint.z /= ratPoint.z;
        return resPoint;
    }

    double getResultWeight(const Point3d& inPt) const
    {
        double sumW=0;
        for (size_t i=0; i<positionsV.size(); i++)
        {
            Point3d aPt = positionsV[i];
            Point3d sPt = densityKernel;

            sPt.x *= std::exp(aPt.z);
            sPt.y *= std::exp(aPt.z);

            aPt -= inPt;

            aPt.x /= sPt.x;
            aPt.y /= sPt.y;
            aPt.z /= sPt.z;

            sumW+=(weightsV[i])*std::exp(-(aPt.dot(aPt))/2)/std::sqrt(sPt.dot(Point3d(1,1,1)));
        }
        return sumW;
    }

    Point3d moveToMode(Point3d aPt) const
    {
        Point3d bPt;
        for (int i = 0; i<iterMax; i++)
        {
            bPt = aPt;
            aPt = getNewValue(bPt);
            if ( getDistance(aPt, bPt) <= modeEps )
            {
                break;
            }
        }
        return aPt;
    }

    double getDistance(Point3d p1, Point3d p2) const
    {
        Point3d ns = densityKernel;
        ns.x *= std::exp(p2.z);
        ns.y *= std::exp(p2.z);
        p2 -= p1;
        p2.x /= ns.x;
        p2.y /= ns.y;
        p2.z /= ns.z;
        return p2.dot(p2);
    }
};
//new grouping function with using meanshift
static void groupRectangles_meanshift(std::vector<Rect>& rectList, double detectThreshold, std::vector<double>* foundWeights,
                                      std::vector<double>& scales, Size winDetSize)
{
    int detectionCount = (int)rectList.size();
    std::vector<Point3d> hits(detectionCount), resultHits;
    std::vector<double> hitWeights(detectionCount), resultWeights;
    Point2d hitCenter;

    for (int i=0; i < detectionCount; i++)
    {
        hitWeights[i] = (*foundWeights)[i];
        hitCenter = (rectList[i].tl() + rectList[i].br())*(0.5); //center of rectangles
        hits[i] = Point3d(hitCenter.x, hitCenter.y, std::log(scales[i]));
    }

    rectList.clear();
    if (foundWeights)
        foundWeights->clear();

    double logZ = std::log(1.3);
    Point3d smothing(8, 16, logZ);

    MeanshiftGrouping msGrouping(smothing, hits, hitWeights, 1e-5, 100);

    msGrouping.getModes(resultHits, resultWeights, 1);

    for (unsigned i=0; i < resultHits.size(); ++i)
    {

        double scale = std::exp(resultHits[i].z);
        hitCenter.x = resultHits[i].x;
        hitCenter.y = resultHits[i].y;
        Size s( int(winDetSize.width * scale), int(winDetSize.height * scale) );
        Rect resultRect( int(hitCenter.x-s.width/2), int(hitCenter.y-s.height/2),
            int(s.width), int(s.height) );

        if (resultWeights[i] > detectThreshold)
        {
            rectList.push_back(resultRect);
            foundWeights->push_back(resultWeights[i]);
        }
    }
}

void groupRectangles(std::vector<Rect>& rectList, int groupThreshold, double eps)
{
    groupRectangles(rectList, groupThreshold, eps, 0, 0);
}

void groupRectangles(std::vector<Rect>& rectList, std::vector<int>& weights, int groupThreshold, double eps)
{
    groupRectangles(rectList, groupThreshold, eps, &weights, 0);
}
//used for cascade detection algorithm for ROC-curve calculating
void groupRectangles(std::vector<Rect>& rectList, std::vector<int>& rejectLevels, std::vector<double>& levelWeights, int groupThreshold, double eps)
{
    groupRectangles(rectList, groupThreshold, eps, &rejectLevels, &levelWeights);
}
//can be used for HOG detection algorithm only
void groupRectangles_meanshift(std::vector<Rect>& rectList, std::vector<double>& foundWeights,
                               std::vector<double>& foundScales, double detectThreshold, Size winDetSize)
{
    groupRectangles_meanshift(rectList, detectThreshold, &foundWeights, foundScales, winDetSize);
}


FeatureEvaluator::~FeatureEvaluator() {}
bool FeatureEvaluator::read(const FileNode&) {return true;}
Ptr<FeatureEvaluator> FeatureEvaluator::clone() const { return Ptr<FeatureEvaluator>(); }
int FeatureEvaluator::getFeatureType() const {return -1;}
bool FeatureEvaluator::setImage(InputArray, Size, Size) {return true;}
bool FeatureEvaluator::setWindow(Point) { return true; }
double FeatureEvaluator::calcOrd(int) const { return 0.; }
int FeatureEvaluator::calcCat(int) const { return 0; }

//----------------------------------------------  HaarEvaluator ---------------------------------------

bool HaarEvaluator::Feature :: read( const FileNode& node )
{
    FileNode rnode = node[CC_RECTS];
    FileNodeIterator it = rnode.begin(), it_end = rnode.end();

    int ri;
    for( ri = 0; ri < RECT_NUM; ri++ )
    {
        rect[ri].r = Rect();
        rect[ri].weight = 0.f;
    }

    for(ri = 0; it != it_end; ++it, ri++)
    {
        FileNodeIterator it2 = (*it).begin();
        it2 >> rect[ri].r.x >> rect[ri].r.y >>
            rect[ri].r.width >> rect[ri].r.height >> rect[ri].weight;
    }

    tilted = (int)node[CC_TILTED] != 0;
    return true;
}

HaarEvaluator::HaarEvaluator()
{
    optfeaturesPtr = 0;
    pwin = 0;
}
HaarEvaluator::~HaarEvaluator()
{
}

bool HaarEvaluator::read(const FileNode& node)
{
    size_t i, n = node.size();
    CV_Assert(n > 0);
    if(features.empty())
        features = makePtr<std::vector<Feature> >();
    if(optfeatures.empty())
        optfeatures = makePtr<std::vector<OptFeature> >();
    features->resize(n);
    FileNodeIterator it = node.begin();
    hasTiltedFeatures = false;
    std::vector<Feature>& ff = *features;
    sumSize0 = Size();
    ufbuf.release();

    for(i = 0; i < n; i++, ++it)
    {
        if(!ff[i].read(*it))
            return false;
        if( ff[i].tilted )
            hasTiltedFeatures = true;
    }
    return true;
}

Ptr<FeatureEvaluator> HaarEvaluator::clone() const
{
    Ptr<HaarEvaluator> ret = makePtr<HaarEvaluator>();
    ret->origWinSize = origWinSize;
    ret->features = features;
    ret->optfeatures = optfeatures;
    ret->optfeaturesPtr = optfeatures->empty() ? 0 : &(*(ret->optfeatures))[0];
    ret->hasTiltedFeatures = hasTiltedFeatures;
    ret->sum0 = sum0; ret->sqsum0 = sqsum0;
    ret->sum = sum; ret->sqsum = sqsum;
    ret->usum0 = usum0; ret->usqsum0 = usqsum0; ret->ufbuf = ufbuf;
    ret->normrect = normrect;
    memcpy( ret->nofs, nofs, 4*sizeof(nofs[0]) );
    ret->pwin = pwin;
    ret->varianceNormFactor = varianceNormFactor;
    return ret;
}

bool HaarEvaluator::setImage( InputArray _image, Size _origWinSize, Size _sumSize )
{
    Size imgsz = _image.size();
    int cols = imgsz.width, rows = imgsz.height;

    if (imgsz.width < origWinSize.width || imgsz.height < origWinSize.height)
        return false;

    origWinSize = _origWinSize;
    normrect = Rect(1, 1, origWinSize.width-2, origWinSize.height-2);

    int rn = _sumSize.height, cn = _sumSize.width, rn_scale = hasTiltedFeatures ? 2 : 1;
    int sumStep, tofs = 0;
    CV_Assert(rn >= rows+1 && cn >= cols+1);

    if( _image.isUMat() )
    {
        usum0.create(rn*rn_scale, cn, CV_32S);
        usqsum0.create(rn, cn, CV_32S);
        usum = UMat(usum0, Rect(0, 0, cols+1, rows+1));
        usqsum = UMat(usqsum0, Rect(0, 0, cols, rows));

        if( hasTiltedFeatures )
        {
            UMat utilted(usum0, Rect(0, _sumSize.height, cols+1, rows+1));
            integral(_image, usum, noArray(), utilted, CV_32S);
            tofs = (int)((utilted.offset - usum.offset)/sizeof(int));
        }
        else
        {
            integral(_image, usum, noArray(), noArray(), CV_32S);
        }

        sqrBoxFilter(_image, usqsum, CV_32S,
                     Size(normrect.width, normrect.height),
                     Point(0, 0), false);
        /*sqrBoxFilter(_image.getMat(), sqsum, CV_32S,
                     Size(normrect.width, normrect.height),
                     Point(0, 0), false);
        sqsum.copyTo(usqsum);*/
        sumStep = (int)(usum.step/usum.elemSize());
    }
    else
    {
        sum0.create(rn*rn_scale, cn, CV_32S);
        sqsum0.create(rn, cn, CV_32S);
        sum = sum0(Rect(0, 0, cols+1, rows+1));
        sqsum = sqsum0(Rect(0, 0, cols, rows));

        if( hasTiltedFeatures )
        {
            Mat tilted = sum0(Rect(0, _sumSize.height, cols+1, rows+1));
            integral(_image, sum, noArray(), tilted, CV_32S);
            tofs = (int)((tilted.data - sum.data)/sizeof(int));
        }
        else
            integral(_image, sum, noArray(), noArray(), CV_32S);
        sqrBoxFilter(_image, sqsum, CV_32S,
                     Size(normrect.width, normrect.height),
                     Point(0, 0), false);
        sumStep = (int)(sum.step/sum.elemSize());
    }

    CV_SUM_OFS( nofs[0], nofs[1], nofs[2], nofs[3], 0, normrect, sumStep );

    size_t fi, nfeatures = features->size();
    const std::vector<Feature>& ff = *features;

    if( sumSize0 != _sumSize )
    {
        optfeatures->resize(nfeatures);
        optfeaturesPtr = &(*optfeatures)[0];
        for( fi = 0; fi < nfeatures; fi++ )
            optfeaturesPtr[fi].setOffsets( ff[fi], sumStep, tofs );
    }
    if( _image.isUMat() && (sumSize0 != _sumSize || ufbuf.empty()) )
        copyVectorToUMat(*optfeatures, ufbuf);
    sumSize0 = _sumSize;

    return true;
}


bool  HaarEvaluator::setWindow( Point pt )
{
    if( pt.x < 0 || pt.y < 0 ||
        pt.x + origWinSize.width >= sum.cols ||
        pt.y + origWinSize.height >= sum.rows )
        return false;

    const int* p = &sum.at<int>(pt);
    int valsum = CALC_SUM_OFS(nofs, p);
    double valsqsum = sqsum.at<int>(pt.y + normrect.y, pt.x + normrect.x);

    double nf = (double)normrect.area() * valsqsum - (double)valsum * valsum;
    if( nf > 0. )
        nf = std::sqrt(nf);
    else
        nf = 1.;
    varianceNormFactor = 1./nf;
    pwin = p;

    return true;
}

Rect HaarEvaluator::getNormRect() const
{
    return normrect;
}

void HaarEvaluator::getUMats(std::vector<UMat>& bufs)
{
    bufs.clear();
    bufs.push_back(usum);
    bufs.push_back(usqsum);
    bufs.push_back(ufbuf);
}

//----------------------------------------------  LBPEvaluator -------------------------------------
bool LBPEvaluator::Feature :: read(const FileNode& node )
{
    FileNode rnode = node[CC_RECT];
    FileNodeIterator it = rnode.begin();
    it >> rect.x >> rect.y >> rect.width >> rect.height;
    return true;
}

LBPEvaluator::LBPEvaluator()
{
    features = makePtr<std::vector<Feature> >();
    optfeatures = makePtr<std::vector<OptFeature> >();
}
LBPEvaluator::~LBPEvaluator()
{
}

bool LBPEvaluator::read( const FileNode& node )
{
    features->resize(node.size());
    optfeaturesPtr = &(*optfeatures)[0];
    FileNodeIterator it = node.begin(), it_end = node.end();
    std::vector<Feature>& ff = *features;
    for(int i = 0; it != it_end; ++it, i++)
    {
        if(!ff[i].read(*it))
            return false;
    }
    return true;
}

Ptr<FeatureEvaluator> LBPEvaluator::clone() const
{
    Ptr<LBPEvaluator> ret = makePtr<LBPEvaluator>();
    ret->origWinSize = origWinSize;
    ret->features = features;
    ret->optfeatures = optfeatures;
    ret->optfeaturesPtr = ret->optfeatures.empty() ? 0 : &(*ret->optfeatures)[0];
    ret->sum0 = sum0, ret->sum = sum;
    ret->pwin = pwin;
    return ret;
}

bool LBPEvaluator::setImage( InputArray _image, Size _origWinSize, Size _sumSize )
{
    Size imgsz = _image.size();
    int cols = imgsz.width, rows = imgsz.height;

    if (imgsz.width < origWinSize.width || imgsz.height < origWinSize.height)
        return false;

    origWinSize = _origWinSize;

    int rn = _sumSize.height, cn = _sumSize.width;
    int sumStep;
    CV_Assert(rn >= rows+1 && cn >= cols+1);

    if( _image.isUMat() )
    {
        usum0.create(rn, cn, CV_32S);
        usum = UMat(usum0, Rect(0, 0, cols+1, rows+1));

        integral(_image, usum, noArray(), noArray(), CV_32S);
        sumStep = (int)(usum.step/usum.elemSize());
    }
    else
    {
        sum0.create(rn, cn, CV_32S);
        sum = sum0(Rect(0, 0, cols+1, rows+1));

        integral(_image, sum, noArray(), noArray(), CV_32S);
        sumStep = (int)(sum.step/sum.elemSize());
    }

    size_t fi, nfeatures = features->size();
    const std::vector<Feature>& ff = *features;

    if( sumSize0 != _sumSize )
    {
        optfeatures->resize(nfeatures);
        optfeaturesPtr = &(*optfeatures)[0];
        for( fi = 0; fi < nfeatures; fi++ )
            optfeaturesPtr[fi].setOffsets( ff[fi], sumStep );
    }
    if( _image.isUMat() && (sumSize0 != _sumSize || ufbuf.empty()) )
        copyVectorToUMat(*optfeatures, ufbuf);
    sumSize0 = _sumSize;

    return true;
}

bool LBPEvaluator::setWindow( Point pt )
{
    if( pt.x < 0 || pt.y < 0 ||
        pt.x + origWinSize.width >= sum.cols ||
        pt.y + origWinSize.height >= sum.rows )
        return false;
    pwin = &sum.at<int>(pt);
    return true;
}


void LBPEvaluator::getUMats(std::vector<UMat>& bufs)
{
    bufs.clear();
    bufs.push_back(usum);
    bufs.push_back(ufbuf);
}

//----------------------------------------------  HOGEvaluator ---------------------------------------
bool HOGEvaluator::Feature :: read( const FileNode& node )
{
    FileNode rnode = node[CC_RECT];
    FileNodeIterator it = rnode.begin();
    it >> rect[0].x >> rect[0].y >> rect[0].width >> rect[0].height >> featComponent;
    rect[1].x = rect[0].x + rect[0].width;
    rect[1].y = rect[0].y;
    rect[2].x = rect[0].x;
    rect[2].y = rect[0].y + rect[0].height;
    rect[3].x = rect[0].x + rect[0].width;
    rect[3].y = rect[0].y + rect[0].height;
    rect[1].width = rect[2].width = rect[3].width = rect[0].width;
    rect[1].height = rect[2].height = rect[3].height = rect[0].height;
    return true;
}

HOGEvaluator::HOGEvaluator()
{
    features = makePtr<std::vector<Feature> >();
}

HOGEvaluator::~HOGEvaluator()
{
}

bool HOGEvaluator::read( const FileNode& node )
{
    features->resize(node.size());
    featuresPtr = &(*features)[0];
    FileNodeIterator it = node.begin(), it_end = node.end();
    for(int i = 0; it != it_end; ++it, i++)
    {
        if(!featuresPtr[i].read(*it))
            return false;
    }
    return true;
}

Ptr<FeatureEvaluator> HOGEvaluator::clone() const
{
    Ptr<HOGEvaluator> ret = makePtr<HOGEvaluator>();
    ret->origWinSize = origWinSize;
    ret->features = features;
    ret->featuresPtr = &(*ret->features)[0];
    ret->offset = offset;
    ret->hist = hist;
    ret->normSum = normSum;
    return ret;
}

bool HOGEvaluator::setImage( InputArray _image, Size winSize, Size )
{
    Mat image = _image.getMat();
    int rows = image.rows + 1;
    int cols = image.cols + 1;
    origWinSize = winSize;
    if( image.cols < origWinSize.width || image.rows < origWinSize.height )
        return false;
    hist.clear();
    for( int bin = 0; bin < Feature::BIN_NUM; bin++ )
    {
        hist.push_back( Mat(rows, cols, CV_32FC1) );
    }
    normSum.create( rows, cols, CV_32FC1 );

    integralHistogram( image, hist, normSum, Feature::BIN_NUM );

    size_t featIdx, featCount = features->size();

    for( featIdx = 0; featIdx < featCount; featIdx++ )
    {
        featuresPtr[featIdx].updatePtrs( hist, normSum );
    }
    return true;
}

bool HOGEvaluator::setWindow(Point pt)
{
    if( pt.x < 0 || pt.y < 0 ||
        pt.x + origWinSize.width >= hist[0].cols-2 ||
        pt.y + origWinSize.height >= hist[0].rows-2 )
        return false;
    offset = pt.y * ((int)hist[0].step/sizeof(float)) + pt.x;
    return true;
}

void HOGEvaluator::integralHistogram(const Mat &img, std::vector<Mat> &histogram, Mat &norm, int nbins) const
{
    CV_Assert( img.type() == CV_8U || img.type() == CV_8UC3 );
    int x, y, binIdx;

    Size gradSize(img.size());
    Size histSize(histogram[0].size());
    Mat grad(gradSize, CV_32F);
    Mat qangle(gradSize, CV_8U);

    AutoBuffer<int> mapbuf(gradSize.width + gradSize.height + 4);
    int* xmap = (int*)mapbuf + 1;
    int* ymap = xmap + gradSize.width + 2;

    const int borderType = (int)BORDER_REPLICATE;

    for( x = -1; x < gradSize.width + 1; x++ )
        xmap[x] = borderInterpolate(x, gradSize.width, borderType);
    for( y = -1; y < gradSize.height + 1; y++ )
        ymap[y] = borderInterpolate(y, gradSize.height, borderType);

    int width = gradSize.width;
    AutoBuffer<float> _dbuf(width*4);
    float* dbuf = _dbuf;
    Mat Dx(1, width, CV_32F, dbuf);
    Mat Dy(1, width, CV_32F, dbuf + width);
    Mat Mag(1, width, CV_32F, dbuf + width*2);
    Mat Angle(1, width, CV_32F, dbuf + width*3);

    float angleScale = (float)(nbins/CV_PI);

    for( y = 0; y < gradSize.height; y++ )
    {
        const uchar* currPtr = img.data + img.step*ymap[y];
        const uchar* prevPtr = img.data + img.step*ymap[y-1];
        const uchar* nextPtr = img.data + img.step*ymap[y+1];
        float* gradPtr = (float*)grad.ptr(y);
        uchar* qanglePtr = (uchar*)qangle.ptr(y);

        for( x = 0; x < width; x++ )
        {
            dbuf[x] = (float)(currPtr[xmap[x+1]] - currPtr[xmap[x-1]]);
            dbuf[width + x] = (float)(nextPtr[xmap[x]] - prevPtr[xmap[x]]);
        }
        cartToPolar( Dx, Dy, Mag, Angle, false );
        for( x = 0; x < width; x++ )
        {
            float mag = dbuf[x+width*2];
            float angle = dbuf[x+width*3];
            angle = angle*angleScale - 0.5f;
            int bidx = cvFloor(angle);
            angle -= bidx;
            if( bidx < 0 )
                bidx += nbins;
            else if( bidx >= nbins )
                bidx -= nbins;

            qanglePtr[x] = (uchar)bidx;
            gradPtr[x] = mag;
        }
    }
    integral(grad, norm, grad.depth());

    float* histBuf;
    const float* magBuf;
    const uchar* binsBuf;

    int binsStep = (int)( qangle.step / sizeof(uchar) );
    int histStep = (int)( histogram[0].step / sizeof(float) );
    int magStep = (int)( grad.step / sizeof(float) );
    for( binIdx = 0; binIdx < nbins; binIdx++ )
    {
        histBuf = (float*)histogram[binIdx].data;
        magBuf = (const float*)grad.data;
        binsBuf = (const uchar*)qangle.data;

        memset( histBuf, 0, histSize.width * sizeof(histBuf[0]) );
        histBuf += histStep + 1;
        for( y = 0; y < qangle.rows; y++ )
        {
            histBuf[-1] = 0.f;
            float strSum = 0.f;
            for( x = 0; x < qangle.cols; x++ )
            {
                if( binsBuf[x] == binIdx )
                    strSum += magBuf[x];
                histBuf[x] = histBuf[-histStep + x] + strSum;
            }
            histBuf += histStep;
            binsBuf += binsStep;
            magBuf += magStep;
        }
    }
}

Ptr<FeatureEvaluator> FeatureEvaluator::create( int featureType )
{
    return featureType == HAAR ? Ptr<FeatureEvaluator>(new HaarEvaluator) :
        featureType == LBP ? Ptr<FeatureEvaluator>(new LBPEvaluator) :
        featureType == HOG ? Ptr<FeatureEvaluator>(new HOGEvaluator) :
        Ptr<FeatureEvaluator>();
}

//---------------------------------------- Classifier Cascade --------------------------------------------

CascadeClassifierImpl::CascadeClassifierImpl()
{
}

CascadeClassifierImpl::~CascadeClassifierImpl()
{
}

bool CascadeClassifierImpl::empty() const
{
    return !oldCascade && data.stages.empty();
}

bool CascadeClassifierImpl::load(const String& filename)
{
    oldCascade.release();
    data = Data();
    featureEvaluator.release();

    FileStorage fs(filename, FileStorage::READ);
    if( !fs.isOpened() )
        return false;

    if( read_(fs.getFirstTopLevelNode()) )
        return true;

    fs.release();

    oldCascade.reset((CvHaarClassifierCascade*)cvLoad(filename.c_str(), 0, 0, 0));
    return !oldCascade.empty();
}

void CascadeClassifierImpl::read(const FileNode& node)
{
    read_(node);
}

int CascadeClassifierImpl::runAt( Ptr<FeatureEvaluator>& evaluator, Point pt, double& weight )
{
    CV_Assert( !oldCascade );

    assert( data.featureType == FeatureEvaluator::HAAR ||
            data.featureType == FeatureEvaluator::LBP ||
            data.featureType == FeatureEvaluator::HOG );

    if( !evaluator->setWindow(pt) )
        return -1;
    if( data.isStumpBased() )
    {
        if( data.featureType == FeatureEvaluator::HAAR )
            return predictOrderedStump<HaarEvaluator>( *this, evaluator, weight );
        else if( data.featureType == FeatureEvaluator::LBP )
            return predictCategoricalStump<LBPEvaluator>( *this, evaluator, weight );
        else if( data.featureType == FeatureEvaluator::HOG )
            return predictOrderedStump<HOGEvaluator>( *this, evaluator, weight );
        else
            return -2;
    }
    else
    {
        if( data.featureType == FeatureEvaluator::HAAR )
            return predictOrdered<HaarEvaluator>( *this, evaluator, weight );
        else if( data.featureType == FeatureEvaluator::LBP )
            return predictCategorical<LBPEvaluator>( *this, evaluator, weight );
        else if( data.featureType == FeatureEvaluator::HOG )
            return predictOrdered<HOGEvaluator>( *this, evaluator, weight );
        else
            return -2;
    }
}

void CascadeClassifierImpl::setMaskGenerator(const Ptr<MaskGenerator>& _maskGenerator)
{
    maskGenerator=_maskGenerator;
}
Ptr<CascadeClassifierImpl::MaskGenerator> CascadeClassifierImpl::getMaskGenerator()
{
    return maskGenerator;
}

Ptr<BaseCascadeClassifier::MaskGenerator> createFaceDetectionMaskGenerator()
{
#ifdef HAVE_TEGRA_OPTIMIZATION
    return tegra::getCascadeClassifierMaskGenerator(*this);
#else
    return Ptr<BaseCascadeClassifier::MaskGenerator>();
#endif
}

class CascadeClassifierInvoker : public ParallelLoopBody
{
public:
    CascadeClassifierInvoker( CascadeClassifierImpl& _cc, Size _sz1, int _stripSize, int _yStep, double _factor,
        std::vector<Rect>& _vec, std::vector<int>& _levels, std::vector<double>& _weights, bool outputLevels, const Mat& _mask, Mutex* _mtx)
    {
        classifier = &_cc;
        processingRectSize = _sz1;
        stripSize = _stripSize;
        yStep = _yStep;
        scalingFactor = _factor;
        rectangles = &_vec;
        rejectLevels = outputLevels ? &_levels : 0;
        levelWeights = outputLevels ? &_weights : 0;
        mask = _mask;
        mtx = _mtx;
    }

    void operator()(const Range& range) const
    {
        Ptr<FeatureEvaluator> evaluator = classifier->featureEvaluator->clone();

        Size winSize(cvRound(classifier->data.origWinSize.width * scalingFactor),
                     cvRound(classifier->data.origWinSize.height * scalingFactor));

        int y1 = range.start * stripSize;
        int y2 = std::min(range.end * stripSize, processingRectSize.height);
        for( int y = y1; y < y2; y += yStep )
        {
            for( int x = 0; x < processingRectSize.width; x += yStep )
            {
                if ( (!mask.empty()) && (mask.at<uchar>(Point(x,y))==0)) {
                    continue;
                }

                double gypWeight;
                int result = classifier->runAt(evaluator, Point(x, y), gypWeight);

#if defined (LOG_CASCADE_STATISTIC)

                logger.setPoint(Point(x, y), result);
#endif
                if( rejectLevels )
                {
                    if( result == 1 )
                        result =  -(int)classifier->data.stages.size();
                    if( classifier->data.stages.size() + result == 0 )
                    {
                        mtx->lock();
                        rectangles->push_back(Rect(cvRound(x*scalingFactor), cvRound(y*scalingFactor), winSize.width, winSize.height));
                        rejectLevels->push_back(-result);
                        levelWeights->push_back(gypWeight);
                        mtx->unlock();
                    }
                }
                else if( result > 0 )
                {
                    mtx->lock();
                    rectangles->push_back(Rect(cvRound(x*scalingFactor), cvRound(y*scalingFactor),
                                               winSize.width, winSize.height));
                    mtx->unlock();
                }
                if( result == 0 )
                    x += yStep;
            }
        }
    }

    CascadeClassifierImpl* classifier;
    std::vector<Rect>* rectangles;
    Size processingRectSize;
    int stripSize, yStep;
    double scalingFactor;
    std::vector<int> *rejectLevels;
    std::vector<double> *levelWeights;
    Mat mask;
    Mutex* mtx;
};

struct getRect { Rect operator ()(const CvAvgComp& e) const { return e.rect; } };
struct getNeighbors { int operator ()(const CvAvgComp& e) const { return e.neighbors; } };


bool CascadeClassifierImpl::detectSingleScale( InputArray _image, Size processingRectSize,
                                           int yStep, double factor, std::vector<Rect>& candidates,
                                           std::vector<int>& levels, std::vector<double>& weights,
                                           Size sumSize0, bool outputRejectLevels )
{
    if( !featureEvaluator->setImage(_image, data.origWinSize, sumSize0) )
        return false;

#if defined (LOG_CASCADE_STATISTIC)
    logger.setImage(image);
#endif

    Mat currentMask;
    if (maskGenerator) {
        Mat image = _image.getMat();
        currentMask=maskGenerator->generateMask(image);
    }

    std::vector<Rect> candidatesVector;
    std::vector<int> rejectLevels;
    std::vector<double> levelWeights;

    int stripCount, stripSize;

    const int PTS_PER_THREAD = 1000;
    stripCount = ((processingRectSize.width/yStep)*(processingRectSize.height + yStep-1)/yStep + PTS_PER_THREAD/2)/PTS_PER_THREAD;
    stripCount = std::min(std::max(stripCount, 1), 100);
    stripSize = (((processingRectSize.height + stripCount - 1)/stripCount + yStep-1)/yStep)*yStep;

    if( outputRejectLevels )
    {
        parallel_for_(Range(0, stripCount), CascadeClassifierInvoker( *this, processingRectSize, stripSize, yStep, factor,
            candidatesVector, rejectLevels, levelWeights, true, currentMask, &mtx));
        levels.insert( levels.end(), rejectLevels.begin(), rejectLevels.end() );
        weights.insert( weights.end(), levelWeights.begin(), levelWeights.end() );
    }
    else
    {
         parallel_for_(Range(0, stripCount), CascadeClassifierInvoker( *this, processingRectSize, stripSize, yStep, factor,
            candidatesVector, rejectLevels, levelWeights, false, currentMask, &mtx));
    }
    candidates.insert( candidates.end(), candidatesVector.begin(), candidatesVector.end() );

#if defined (LOG_CASCADE_STATISTIC)
    logger.write();
#endif

    return true;
}


bool CascadeClassifierImpl::ocl_detectSingleScale( InputArray _image, Size processingRectSize,
                                                   int yStep, double factor, Size sumSize0 )
{
    int featureType = getFeatureType();
    std::vector<UMat> bufs;
    size_t globalsize[] = { processingRectSize.width/yStep, processingRectSize.height/yStep };
    bool ok = false;

    if( ustages.empty() )
    {
        copyVectorToUMat(data.stages, ustages);
        copyVectorToUMat(data.stumps, ustumps);
        if( !data.subsets.empty() )
            copyVectorToUMat(data.subsets, usubsets);
    }

    if( featureType == FeatureEvaluator::HAAR )
    {
        Ptr<HaarEvaluator> haar = featureEvaluator.dynamicCast<HaarEvaluator>();
        if( haar.empty() )
            return false;

        haar->setImage(_image, data.origWinSize, sumSize0);
        if( haarKernel.empty() )
        {
            haarKernel.create("runHaarClassifierStump", ocl::objdetect::cascadedetect_oclsrc, "");
            if( haarKernel.empty() )
                return false;
        }

        haar->getUMats(bufs);
        Rect normrect = haar->getNormRect();

        haarKernel.args(ocl::KernelArg::ReadOnlyNoSize(bufs[0]), // sum
                        ocl::KernelArg::ReadOnlyNoSize(bufs[1]), // sqsum
                        ocl::KernelArg::PtrReadOnly(bufs[2]), // optfeatures

                        // cascade classifier
                        (int)data.stages.size(),
                        ocl::KernelArg::PtrReadOnly(ustages),
                        ocl::KernelArg::PtrReadOnly(ustumps),

                        ocl::KernelArg::PtrWriteOnly(ufacepos), // positions
                        processingRectSize,
                        yStep, (float)factor,
                        normrect, data.origWinSize, (int)MAX_FACES);
        ok = haarKernel.run(2, globalsize, 0, true);
    }
    else if( featureType == FeatureEvaluator::LBP )
    {
        Ptr<LBPEvaluator> lbp = featureEvaluator.dynamicCast<LBPEvaluator>();
        if( lbp.empty() )
            return false;

        lbp->setImage(_image, data.origWinSize, sumSize0);
        if( lbpKernel.empty() )
        {
            lbpKernel.create("runLBPClassifierStump", ocl::objdetect::cascadedetect_oclsrc, "");
            if( lbpKernel.empty() )
                return false;
        }

        lbp->getUMats(bufs);

        int subsetSize = (data.ncategories + 31)/32;
        lbpKernel.args(ocl::KernelArg::ReadOnlyNoSize(bufs[0]), // sum
                        ocl::KernelArg::PtrReadOnly(bufs[1]), // optfeatures

                        // cascade classifier
                        (int)data.stages.size(),
                        ocl::KernelArg::PtrReadOnly(ustages),
                        ocl::KernelArg::PtrReadOnly(ustumps),
                        ocl::KernelArg::PtrReadOnly(usubsets),
                        subsetSize,

                        ocl::KernelArg::PtrWriteOnly(ufacepos), // positions
                        processingRectSize,
                        yStep, (float)factor,
                        data.origWinSize, (int)MAX_FACES);
        ok = lbpKernel.run(2, globalsize, 0, true);
    }
    //CV_Assert(ok);
    return ok;
}

bool CascadeClassifierImpl::isOldFormatCascade() const
{
    return !oldCascade.empty();
}

int CascadeClassifierImpl::getFeatureType() const
{
    return featureEvaluator->getFeatureType();
}

Size CascadeClassifierImpl::getOriginalWindowSize() const
{
    return data.origWinSize;
}

void* CascadeClassifierImpl::getOldCascade()
{
    return oldCascade;
}

static void detectMultiScaleOldFormat( const Mat& image, Ptr<CvHaarClassifierCascade> oldCascade,
                                       std::vector<Rect>& objects,
                                       std::vector<int>& rejectLevels,
                                       std::vector<double>& levelWeights,
                                       std::vector<CvAvgComp>& vecAvgComp,
                                       double scaleFactor, int minNeighbors,
                                       int flags, Size minObjectSize, Size maxObjectSize,
                                       bool outputRejectLevels = false )
{
    MemStorage storage(cvCreateMemStorage(0));
    CvMat _image = image;
    CvSeq* _objects = cvHaarDetectObjectsForROC( &_image, oldCascade, storage, rejectLevels, levelWeights, scaleFactor,
                                                 minNeighbors, flags, minObjectSize, maxObjectSize, outputRejectLevels );
    Seq<CvAvgComp>(_objects).copyTo(vecAvgComp);
    objects.resize(vecAvgComp.size());
    std::transform(vecAvgComp.begin(), vecAvgComp.end(), objects.begin(), getRect());
}


void CascadeClassifierImpl::detectMultiScaleNoGrouping( InputArray _image, std::vector<Rect>& candidates,
                                                    std::vector<int>& rejectLevels, std::vector<double>& levelWeights,
                                                    double scaleFactor, Size minObjectSize, Size maxObjectSize,
                                                    bool outputRejectLevels )
{
    int featureType = getFeatureType();
    Size imgsz = _image.size();
    int imgtype = _image.type();

    Mat grayImage, imageBuffer;

    candidates.clear();
    rejectLevels.clear();
    levelWeights.clear();

    if( maxObjectSize.height == 0 || maxObjectSize.width == 0 )
        maxObjectSize = imgsz;

    bool use_ocl = ocl::useOpenCL() &&
        (featureType == FeatureEvaluator::HAAR ||
         featureType == FeatureEvaluator::LBP) &&
        ocl::Device::getDefault().type() != ocl::Device::TYPE_CPU &&
        !isOldFormatCascade() &&
        data.isStumpBased() &&
        maskGenerator.empty() &&
        !outputRejectLevels &&
        tryOpenCL;

    if( !use_ocl )
    {
        Mat image = _image.getMat();
        if (maskGenerator)
            maskGenerator->initializeMask(image);

        grayImage = image;
        if( CV_MAT_CN(imgtype) > 1 )
        {
            Mat temp;
            cvtColor(grayImage, temp, COLOR_BGR2GRAY);
            grayImage = temp;
        }

        imageBuffer.create(imgsz.height + 1, imgsz.width + 1, CV_8U);
    }
    else
    {
        UMat uimage = _image.getUMat();
        if( CV_MAT_CN(imgtype) > 1 )
            cvtColor(uimage, ugrayImage, COLOR_BGR2GRAY);
        else
            uimage.copyTo(ugrayImage);
        uimageBuffer.create(imgsz.height + 1, imgsz.width + 1, CV_8U);
    }

    Size sumSize0((imgsz.width + SUM_ALIGN) & -SUM_ALIGN, imgsz.height+1);

    if( use_ocl )
    {
        ufacepos.create(1, MAX_FACES*4 + 1, CV_32S);
        UMat ufacecount(ufacepos, Rect(0,0,1,1));
        ufacecount.setTo(Scalar::all(0));
    }

    for( double factor = 1; ; factor *= scaleFactor )
    {
        Size originalWindowSize = getOriginalWindowSize();

        Size windowSize( cvRound(originalWindowSize.width*factor), cvRound(originalWindowSize.height*factor) );
        Size scaledImageSize( cvRound( imgsz.width/factor ), cvRound( imgsz.height/factor ) );
        Size processingRectSize( scaledImageSize.width - originalWindowSize.width,
                                 scaledImageSize.height - originalWindowSize.height );

        if( processingRectSize.width <= 0 || processingRectSize.height <= 0 )
            break;
        if( windowSize.width > maxObjectSize.width || windowSize.height > maxObjectSize.height )
            break;
        if( windowSize.width < minObjectSize.width || windowSize.height < minObjectSize.height )
            continue;

        int yStep;
        if( getFeatureType() == cv::FeatureEvaluator::HOG )
        {
            yStep = 4;
        }
        else
        {
            yStep = factor > 2. ? 1 : 2;
        }

        if( use_ocl )
        {
            UMat uscaledImage(uimageBuffer, Rect(0, 0, scaledImageSize.width, scaledImageSize.height));
            resize( ugrayImage, uscaledImage, scaledImageSize, 0, 0, INTER_LINEAR );

            if( ocl_detectSingleScale( uscaledImage, processingRectSize, yStep, factor, sumSize0 ) )
                continue;

            /////// if the OpenCL branch has been executed but failed, fall back to CPU: /////

            tryOpenCL = false; // for this cascade do not try OpenCL anymore

            // since we may already have some partial results from OpenCL code (unlikely, but still),
            // we just recursively call the function again, but with tryOpenCL==false it will
            // go with CPU route, so there is no infinite recursion
            detectMultiScaleNoGrouping( _image, candidates, rejectLevels, levelWeights,
                                       scaleFactor, minObjectSize, maxObjectSize,
                                       outputRejectLevels);
            return;
        }
        else
        {
            Mat scaledImage( scaledImageSize, CV_8U, imageBuffer.data );
            resize( grayImage, scaledImage, scaledImageSize, 0, 0, INTER_LINEAR );

            if( !detectSingleScale( scaledImage, processingRectSize, yStep, factor, candidates,
                                    rejectLevels, levelWeights, sumSize0, outputRejectLevels ) )
                break;
        }
    }

    if( use_ocl && tryOpenCL )
    {
        Mat facepos = ufacepos.getMat(ACCESS_READ);
        const int* fptr = facepos.ptr<int>();
        int i, nfaces = fptr[0];
        for( i = 0; i < nfaces; i++ )
        {
            candidates.push_back(Rect(fptr[i*4+1], fptr[i*4+2], fptr[i*4+3], fptr[i*4+4]));
        }
    }
}

void CascadeClassifierImpl::detectMultiScale( InputArray _image, std::vector<Rect>& objects,
                                          std::vector<int>& rejectLevels,
                                          std::vector<double>& levelWeights,
                                          double scaleFactor, int minNeighbors,
                                          int flags, Size minObjectSize, Size maxObjectSize,
                                          bool outputRejectLevels )
{
    CV_Assert( scaleFactor > 1 && _image.depth() == CV_8U );

    if( empty() )
        return;

    if( isOldFormatCascade() )
    {
        Mat image = _image.getMat();
        std::vector<CvAvgComp> fakeVecAvgComp;
        detectMultiScaleOldFormat( image, oldCascade, objects, rejectLevels, levelWeights, fakeVecAvgComp, scaleFactor,
                                   minNeighbors, flags, minObjectSize, maxObjectSize, outputRejectLevels );
    }
    else
    {
        detectMultiScaleNoGrouping( _image, objects, rejectLevels, levelWeights, scaleFactor, minObjectSize, maxObjectSize,
                                    outputRejectLevels );
        const double GROUP_EPS = 0.2;
        if( outputRejectLevels )
        {
            groupRectangles( objects, rejectLevels, levelWeights, minNeighbors, GROUP_EPS );
        }
        else
        {
            groupRectangles( objects, minNeighbors, GROUP_EPS );
        }
    }
}

void CascadeClassifierImpl::detectMultiScale( InputArray _image, std::vector<Rect>& objects,
                                          double scaleFactor, int minNeighbors,
                                          int flags, Size minObjectSize, Size maxObjectSize)
{
    Mat image = _image.getMat();
    std::vector<int> fakeLevels;
    std::vector<double> fakeWeights;
    detectMultiScale( image, objects, fakeLevels, fakeWeights, scaleFactor,
        minNeighbors, flags, minObjectSize, maxObjectSize );
}

void CascadeClassifierImpl::detectMultiScale( InputArray _image, std::vector<Rect>& objects,
                                          std::vector<int>& numDetections, double scaleFactor,
                                          int minNeighbors, int flags, Size minObjectSize,
                                          Size maxObjectSize )
{
    Mat image = _image.getMat();
    CV_Assert( scaleFactor > 1 && image.depth() == CV_8U );

    if( empty() )
        return;

    std::vector<int> fakeLevels;
    std::vector<double> fakeWeights;
    if( isOldFormatCascade() )
    {
        std::vector<CvAvgComp> vecAvgComp;
        detectMultiScaleOldFormat( image, oldCascade, objects, fakeLevels, fakeWeights, vecAvgComp, scaleFactor,
                                   minNeighbors, flags, minObjectSize, maxObjectSize );
        numDetections.resize(vecAvgComp.size());
        std::transform(vecAvgComp.begin(), vecAvgComp.end(), numDetections.begin(), getNeighbors());
    }
    else
    {
        detectMultiScaleNoGrouping( image, objects, fakeLevels, fakeWeights, scaleFactor, minObjectSize, maxObjectSize );
        const double GROUP_EPS = 0.2;
        groupRectangles( objects, numDetections, minNeighbors, GROUP_EPS );
    }
}


CascadeClassifierImpl::Data::Data()
{
    stageType = featureType = ncategories = maxNodesPerTree = 0;
}

bool CascadeClassifierImpl::Data::read(const FileNode &root)
{
    static const float THRESHOLD_EPS = 1e-5f;

    // load stage params
    String stageTypeStr = (String)root[CC_STAGE_TYPE];
    if( stageTypeStr == CC_BOOST )
        stageType = BOOST;
    else
        return false;

    String featureTypeStr = (String)root[CC_FEATURE_TYPE];
    if( featureTypeStr == CC_HAAR )
        featureType = FeatureEvaluator::HAAR;
    else if( featureTypeStr == CC_LBP )
        featureType = FeatureEvaluator::LBP;
    else if( featureTypeStr == CC_HOG )
        featureType = FeatureEvaluator::HOG;

    else
        return false;

    origWinSize.width = (int)root[CC_WIDTH];
    origWinSize.height = (int)root[CC_HEIGHT];
    CV_Assert( origWinSize.height > 0 && origWinSize.width > 0 );

    // load feature params
    FileNode fn = root[CC_FEATURE_PARAMS];
    if( fn.empty() )
        return false;

    ncategories = fn[CC_MAX_CAT_COUNT];
    int subsetSize = (ncategories + 31)/32,
        nodeStep = 3 + ( ncategories>0 ? subsetSize : 1 );

    // load stages
    fn = root[CC_STAGES];
    if( fn.empty() )
        return false;

    stages.reserve(fn.size());
    classifiers.clear();
    nodes.clear();
    stumps.clear();

    FileNodeIterator it = fn.begin(), it_end = fn.end();
    maxNodesPerTree = 0;

    for( int si = 0; it != it_end; si++, ++it )
    {
        FileNode fns = *it;
        Stage stage;
        stage.threshold = (float)fns[CC_STAGE_THRESHOLD] - THRESHOLD_EPS;
        fns = fns[CC_WEAK_CLASSIFIERS];
        if(fns.empty())
            return false;
        stage.ntrees = (int)fns.size();
        stage.first = (int)classifiers.size();
        stages.push_back(stage);
        classifiers.reserve(stages[si].first + stages[si].ntrees);

        FileNodeIterator it1 = fns.begin(), it1_end = fns.end();
        for( ; it1 != it1_end; ++it1 ) // weak trees
        {
            FileNode fnw = *it1;
            FileNode internalNodes = fnw[CC_INTERNAL_NODES];
            FileNode leafValues = fnw[CC_LEAF_VALUES];
            if( internalNodes.empty() || leafValues.empty() )
                return false;

            DTree tree;
            tree.nodeCount = (int)internalNodes.size()/nodeStep;
            maxNodesPerTree = std::max(maxNodesPerTree, tree.nodeCount);

            classifiers.push_back(tree);

            nodes.reserve(nodes.size() + tree.nodeCount);
            leaves.reserve(leaves.size() + leafValues.size());
            if( subsetSize > 0 )
                subsets.reserve(subsets.size() + tree.nodeCount*subsetSize);

            FileNodeIterator internalNodesIter = internalNodes.begin(), internalNodesEnd = internalNodes.end();

            for( ; internalNodesIter != internalNodesEnd; ) // nodes
            {
                DTreeNode node;
                node.left = (int)*internalNodesIter; ++internalNodesIter;
                node.right = (int)*internalNodesIter; ++internalNodesIter;
                node.featureIdx = (int)*internalNodesIter; ++internalNodesIter;
                if( subsetSize > 0 )
                {
                    for( int j = 0; j < subsetSize; j++, ++internalNodesIter )
                        subsets.push_back((int)*internalNodesIter);
                    node.threshold = 0.f;
                }
                else
                {
                    node.threshold = (float)*internalNodesIter; ++internalNodesIter;
                }
                nodes.push_back(node);
            }

            internalNodesIter = leafValues.begin(), internalNodesEnd = leafValues.end();

            for( ; internalNodesIter != internalNodesEnd; ++internalNodesIter ) // leaves
                leaves.push_back((float)*internalNodesIter);
        }
    }

    if( isStumpBased() )
    {
        int nodeOfs = 0, leafOfs = 0;
        size_t nstages = stages.size();
        for( size_t stageIdx = 0; stageIdx < nstages; stageIdx++ )
        {
            const Stage& stage = stages[stageIdx];

            int ntrees = stage.ntrees;
            for( int i = 0; i < ntrees; i++, nodeOfs++, leafOfs+= 2 )
            {
                const DTreeNode& node = nodes[nodeOfs];
                stumps.push_back(Stump(node.featureIdx, node.threshold,
                                       leaves[leafOfs], leaves[leafOfs+1]));
            }
        }
    }

    return true;
}


bool CascadeClassifierImpl::read_(const FileNode& root)
{
    tryOpenCL = true;
    haarKernel = ocl::Kernel();
    lbpKernel = ocl::Kernel();
    ustages.release();
    ustumps.release();
    if( !data.read(root) )
        return false;

    // load features
    featureEvaluator = FeatureEvaluator::create(data.featureType);
    FileNode fn = root[CC_FEATURES];
    if( fn.empty() )
        return false;

    return featureEvaluator->read(fn);
}

template<> void DefaultDeleter<CvHaarClassifierCascade>::operator ()(CvHaarClassifierCascade* obj) const
{ cvReleaseHaarClassifierCascade(&obj); }


BaseCascadeClassifier::~BaseCascadeClassifier()
{
}

CascadeClassifier::CascadeClassifier() {}
CascadeClassifier::CascadeClassifier(const String& filename)
{
    load(filename);
}

CascadeClassifier::~CascadeClassifier()
{
}

bool CascadeClassifier::empty() const
{
    return cc.empty() || cc->empty();
}

bool CascadeClassifier::load( const String& filename )
{
    cc = makePtr<CascadeClassifierImpl>();
    if(!cc->load(filename))
        cc.release();
    return !empty();
}

bool CascadeClassifier::read(const FileNode &root)
{
    Ptr<CascadeClassifierImpl> ccimpl;
    bool ok = ccimpl->read_(root);
    if( ok )
        cc = ccimpl.staticCast<BaseCascadeClassifier>();
    else
        cc.release();
    return ok;
}

void CascadeClassifier::detectMultiScale( InputArray image,
                      CV_OUT std::vector<Rect>& objects,
                      double scaleFactor,
                      int minNeighbors, int flags,
                      Size minSize,
                      Size maxSize )
{
    CV_Assert(!empty());
    cc->detectMultiScale(image, objects, scaleFactor, minNeighbors, flags, minSize, maxSize);
}

void CascadeClassifier::detectMultiScale( InputArray image,
                      CV_OUT std::vector<Rect>& objects,
                      CV_OUT std::vector<int>& numDetections,
                      double scaleFactor,
                      int minNeighbors, int flags,
                      Size minSize, Size maxSize )
{
    CV_Assert(!empty());
    cc->detectMultiScale(image, objects, numDetections,
                         scaleFactor, minNeighbors, flags, minSize, maxSize);
}

void CascadeClassifier::detectMultiScale( InputArray image,
                      CV_OUT std::vector<Rect>& objects,
                      CV_OUT std::vector<int>& rejectLevels,
                      CV_OUT std::vector<double>& levelWeights,
                      double scaleFactor,
                      int minNeighbors, int flags,
                      Size minSize, Size maxSize,
                      bool outputRejectLevels )
{
    CV_Assert(!empty());
    cc->detectMultiScale(image, objects, rejectLevels, levelWeights,
                         scaleFactor, minNeighbors, flags,
                         minSize, maxSize, outputRejectLevels);
}

bool CascadeClassifier::isOldFormatCascade() const
{
    CV_Assert(!empty());
    return cc->isOldFormatCascade();
}

Size CascadeClassifier::getOriginalWindowSize() const
{
    CV_Assert(!empty());
    return cc->getOriginalWindowSize();
}

int CascadeClassifier::getFeatureType() const
{
    CV_Assert(!empty());
    return cc->getFeatureType();
}

void* CascadeClassifier::getOldCascade()
{
    CV_Assert(!empty());
    return cc->getOldCascade();
}

void CascadeClassifier::setMaskGenerator(const Ptr<BaseCascadeClassifier::MaskGenerator>& maskGenerator)
{
    CV_Assert(!empty());
    cc->setMaskGenerator(maskGenerator);
}

Ptr<BaseCascadeClassifier::MaskGenerator> CascadeClassifier::getMaskGenerator()
{
    CV_Assert(!empty());
    return cc->getMaskGenerator();
}

} // namespace cv
