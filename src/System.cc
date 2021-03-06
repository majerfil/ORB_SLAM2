/**
* This file is part of ORB-SLAM2.
*
* Copyright (C) 2014-2016 Raúl Mur-Artal <raulmur at unizar dot es> (University of Zaragoza)
* For more information see <https://github.com/raulmur/ORB_SLAM2>
*
* ORB-SLAM2 is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*
* ORB-SLAM2 is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with ORB-SLAM2. If not, see <http://www.gnu.org/licenses/>.
*/



#include "System.h"
#include "Converter.h"
#include <thread>
#include <pangolin/pangolin.h>
#include <iomanip>
#include <math.h>
char fileName[100];
int numOfMaps=1;
double distLoad=0;
string settings;
bool bUseViewerA;
char fileTrajectory[100];
static bool has_suffix(const std::string &str, const std::string &suffix)
{
    std::size_t index = str.find(suffix, str.size() - suffix.size());
    return (index != std::string::npos);
}

namespace ORB_SLAM2
{

System::System(const string &strVocFile, const string &strSettingsFile, const eSensor sensor,
               const bool bUseViewer, bool is_save_map_):mSensor(sensor), is_save_map(true), mpViewer(static_cast<Viewer*>(NULL)), mbReset(false),
        mbActivateLocalizationMode(false), mbDeactivateLocalizationMode(false)
{
    // Output welcome message
    cout << endl <<
    "ORB-SLAM2 Copyright (C) 2014-2016 Raul Mur-Artal, University of Zaragoza." << endl <<
    "This program comes with ABSOLUTELY NO WARRANTY;" << endl  <<
    "This is free software, and you are welcome to redistribute it" << endl <<
    "under certain conditions. See LICENSE.txt." << endl << endl;
    cout << "Input sensor was set to: ";
    
    if(mSensor==MONOCULAR)
        cout << "Monocular" << endl;
    else if(mSensor==STEREO)
        cout << "Stereo" << endl;
    else if(mSensor==RGBD)
        cout << "RGB-D" << endl;

    //Check settings file
    cv::FileStorage fsSettings(strSettingsFile.c_str(), cv::FileStorage::READ);
    if(!fsSettings.isOpened())
    {
       cerr << "Failed to open settings file at: " << strSettingsFile << endl;
       exit(-1);
    }
    settings=strSettingsFile.c_str();
    bUseViewerA=bUseViewer;
    cv::FileNode mapfilen = fsSettings["Map.mapfile"];
    bool bReuseMap = false;
    if (!mapfilen.empty())
    {	
	
        mapfile = (string)mapfilen;
	sprintf(fileName,"%i%s",numOfMaps,mapfile.c_str());
	cout << "Map name: " << fileName << endl;
    }

    //Load ORB Vocabulary
    cout << endl << "Loading ORB Vocabulary. This could take a while..." << endl;

    mpVocabulary = new ORBVocabulary();
    bool bVocLoad = false; // chose loading method based on file extension
    if (has_suffix(strVocFile, ".txt"))
        bVocLoad = mpVocabulary->loadFromTextFile(strVocFile);
    else if(has_suffix(strVocFile, ".bin"))
        bVocLoad = mpVocabulary->loadFromBinaryFile(strVocFile);
    else
        bVocLoad = false;
    if(!bVocLoad)
    {
        cerr << "Wrong path to vocabulary. " << endl;
        cerr << "Falied to open at: " << strVocFile << endl;
        exit(-1);
    }
    cout << "Vocabulary loaded!" << endl << endl;


    //Create KeyFrame Database
    //Create the Map
    if (!mapfile.empty() && LoadMap(fileName))
    {
        bReuseMap = true;
	
	sprintf(fileName,"%i%s",numOfMaps,mapfile.c_str());
    }
    else
    {
        mpKeyFrameDatabase = new KeyFrameDatabase(mpVocabulary);
        mpMap = new Map();
    }
    //Create Drawers. These are used by the Viewer
    mpFrameDrawer = new FrameDrawer(mpMap, bReuseMap);
    mpMapDrawer = new MapDrawer(mpMap, strSettingsFile);

    //Initialize the Tracking thread
    //(it will live in the main thread of execution, the one that called this constructor)
    mpTracker = new Tracking(this, mpVocabulary, mpFrameDrawer, mpMapDrawer,
                             mpMap, mpKeyFrameDatabase, strSettingsFile, mSensor, bReuseMap);

    //Initialize the Local Mapping thread and launch
    mpLocalMapper = new LocalMapping(mpMap, mSensor==MONOCULAR);
    mptLocalMapping = new thread(&ORB_SLAM2::LocalMapping::Run,mpLocalMapper);

    //Initialize the Loop Closing thread and launch
    mpLoopCloser = new LoopClosing(mpMap, mpKeyFrameDatabase, mpVocabulary, mSensor!=MONOCULAR);
    mptLoopClosing = new thread(&ORB_SLAM2::LoopClosing::Run, mpLoopCloser);

    //Initialize the Viewer thread and launch
    if(bUseViewer)
    {
        mpViewer = new Viewer(this, mpFrameDrawer,mpMapDrawer,mpTracker,strSettingsFile, bReuseMap);
        mptViewer = new thread(&Viewer::Run, mpViewer);
        mpTracker->SetViewer(mpViewer);
    }

    //Set pointers between threads
    mpTracker->SetLocalMapper(mpLocalMapper);
    mpTracker->SetLoopClosing(mpLoopCloser);

    mpLocalMapper->SetTracker(mpTracker);
    mpLocalMapper->SetLoopCloser(mpLoopCloser);

    mpLoopCloser->SetTracker(mpTracker);
    mpLoopCloser->SetLocalMapper(mpLocalMapper);
}

cv::Mat System::TrackStereo(const cv::Mat &imLeft, const cv::Mat &imRight, const double &timestamp)
{
    if(mSensor!=STEREO)
    {
        cerr << "ERROR: you called TrackStereo but input sensor was not set to STEREO." << endl;
        exit(-1);
    }

    // Check mode change
    {
        unique_lock<mutex> lock(mMutexMode);
        if(mbActivateLocalizationMode)
        {
            mpLocalMapper->RequestStop();

            // Wait until Local Mapping has effectively stopped
            while(!mpLocalMapper->isStopped())
            {
                std::this_thread::sleep_for(std::chrono::microseconds(1000));
            }

            mpTracker->InformOnlyTracking(true);
            mbActivateLocalizationMode = false;
        }
        if(mbDeactivateLocalizationMode)
        {
            mpTracker->InformOnlyTracking(false);
            mpLocalMapper->Release();
            mbDeactivateLocalizationMode = false;
        }
    }

    // Check reset
    {
    unique_lock<mutex> lock(mMutexReset);
    if(mbReset)
    {
        mpTracker->Reset();
        mbReset = false;
    }
    }

    cv::Mat Tcw = mpTracker->GrabImageStereo(imLeft,imRight,timestamp);

    unique_lock<mutex> lock2(mMutexState);
    mTrackingState = mpTracker->mState;
    mTrackedMapPoints = mpTracker->mCurrentFrame.mvpMapPoints;
    mTrackedKeyPointsUn = mpTracker->mCurrentFrame.mvKeysUn;
    return Tcw;
}
cv::Mat System::TrackRGBD(const cv::Mat &im, const cv::Mat &depthmap, const double &timestamp)
{
	if(mSensor!=RGBD)
	{
		cerr << "ERROR: you called TrackRGBD but input sensor was not set to RGBD." << endl;
		exit(-1);
	}

	// Check mode change
	{
		unique_lock<mutex> lock(mMutexMode);
		if(mbActivateLocalizationMode)
		{
			mpLocalMapper->RequestStop();

			// Wait until Local Mapping has effectively stopped
			while(!mpLocalMapper->isStopped())
			{
				std::this_thread::sleep_for(std::chrono::microseconds(1000));
			}

			mpTracker->InformOnlyTracking(true);
			mbActivateLocalizationMode = false;
		}
		if(mbDeactivateLocalizationMode)
		{
			mpTracker->InformOnlyTracking(false);
			mpLocalMapper->Release();
			mbDeactivateLocalizationMode = false;
		}
	}

	// Check reset
	{
		unique_lock<mutex> lock(mMutexReset);
		if(mbReset)
		{
			mpTracker->Reset();
			mbReset = false;
		}
	}

	cv::Mat Tcw = mpTracker->GrabImageRGBD(im,depthmap,timestamp);

	unique_lock<mutex> lock2(mMutexState);
	mTrackingState = mpTracker->mState;
	mTrackedMapPoints = mpTracker->mCurrentFrame.mvpMapPoints;
	mTrackedKeyPointsUn = mpTracker->mCurrentFrame.mvKeysUn;
	return Tcw;
}

cv::Mat System::TrackMonocular(const cv::Mat &im, const double &timestamp)
{
	if(mSensor!=MONOCULAR)
	{
		cerr << "ERROR: you called TrackMonocular but input sensor was not set to Monocular." << endl;
		exit(-1);
	}

	// Check mode change
	{
		unique_lock<mutex> lock(mMutexMode);
		if(mbActivateLocalizationMode)
		{
			mpLocalMapper->RequestStop();
			// Wait until Local Mapping has effectively stopped
			while(!mpLocalMapper->isStopped())
			{
				std::this_thread::sleep_for(std::chrono::microseconds(1000));
			}

			mpTracker->InformOnlyTracking(true);
			mbActivateLocalizationMode = false;
		}
		if(mbDeactivateLocalizationMode)
		{
			mpTracker->InformOnlyTracking(false);
			mpLocalMapper->Release();
			mbDeactivateLocalizationMode = false;
		}
	}
	// Check reset
	{
		unique_lock<mutex> lock(mMutexReset);
		if(mbReset)
		{
			if(!mpTracker->mbOnlyTracking){
				SaveMap(fileName);
				sprintf(fileTrajectory,"%iKeyFrameTrajectory.txt",numOfMaps);
				SaveKeyFrameTrajectoryTUM(fileTrajectory);
				numOfMaps++;
				sprintf(fileName,"%i%s",numOfMaps,mapfile.c_str());
				cout << "reset map" << endl;
				mpTracker->Reset();
				mbReset = false;
			}
		}
	}

	cv::Mat Tcw = mpTracker->GrabImageMonocular(im,timestamp);

	unique_lock<mutex> lock2(mMutexState);
	mTrackingState = mpTracker->mState;
	mTrackedMapPoints = mpTracker->mCurrentFrame.mvpMapPoints;
	mTrackedKeyPointsUn = mpTracker->mCurrentFrame.mvKeysUn;
	//Check for end of map, if so, load next map
	if(mpTracker->mbOnlyTracking){
		if(!mpMapDrawer->GetCurrentCameraPose().empty())
		{
			cv::Mat cameraPose = mpMapDrawer->GetCurrentCameraPose();
			double  distCurrent = sqrt(pow(cameraPose.at<float>(0,3),2)+pow(cameraPose.at<float>(1,3),2)+pow(cameraPose.at<float>(2,3),2))+0.1; 
			//cout << "Distance Current " << distCurrent << " Loaded" << distLoad << endl;
			if(distCurrent > distLoad){
				unique_lock<mutex> lock(mMutexReset);
			//	mpTracker->ResetLoad();
				std::this_thread::sleep_for(std::chrono::microseconds(1000));
				numOfMaps++;
				sprintf(fileName,"%i%s",numOfMaps,mapfile.c_str());
				if (LoadMap(fileName))
				{
					bool bReuseMap=true;
					std::this_thread::sleep_for(std::chrono::microseconds(1000));
					//Create Drawers. These are used by the Viewer
		//			mpFrameDrawer = new FrameDrawer(mpMap, bReuseMap);
					mpMapDrawer = new MapDrawer(mpMap, settings);
			//		cout << " 2" << endl;
					//Initialize the Tracking thread
					//(it will live in the main thread of execution, the one that called this constructor)
					mpTracker = new Tracking(this, mpVocabulary, mpFrameDrawer, mpMapDrawer,
							mpMap, mpKeyFrameDatabase, settings, mSensor, bReuseMap);
			//		cout << " 3" << endl;
					//Initialize the Local Mapping thread and launch
					mpLocalMapper = new LocalMapping(mpMap, mSensor==MONOCULAR);
		//			mptLocalMapping = new thread(&ORB_SLAM2::LocalMapping::Run,mpLocalMapper);
			//		cout << "4 " << endl;
					//Initialize the Loop Closing thread and launch
					mpLoopCloser = new LoopClosing(mpMap, mpKeyFrameDatabase, mpVocabulary, mSensor!=MONOCULAR);
				//	mptLoopClosing = new thread(&ORB_SLAM2::LoopClosing::Run, mpLoopCloser);
			//		cout << " 5" << endl;
					//Initialize the Viewer thread and launch
					mpViewer = new Viewer(this, mpFrameDrawer,mpMapDrawer,mpTracker,settings, bReuseMap);
					mpTracker->SetViewer(mpViewer);
			//		cout << "6" << endl;
					//Set pointers between threads
					mpTracker->SetLocalMapper(mpLocalMapper);
					mpTracker->SetLoopClosing(mpLoopCloser);
			//		cout << "7" << endl;	
					mpLocalMapper->SetTracker(mpTracker);
					mpLocalMapper->SetLoopCloser(mpLoopCloser);
			//		cout << "8" << endl;
					mpLoopCloser->SetTracker(mpTracker);
					mpLoopCloser->SetLocalMapper(mpLocalMapper);
					mpTracker->InformOnlyTracking(true);
					distCurrent=0;

				}
			}
		}
	}
	return Tcw;
}
vector<cv::Mat> System::LoadedMapKeyFrames()
{
	unique_lock<mutex> lock2(mMutexState);
    	vector<KeyFrame*> vpKFs = mpMap->GetAllKeyFrames();
	vector<cv::Mat> poses;
	for(unsigned int i=0;i<vpKFs.size();i++){
	poses[i]=vpKFs[i]->GetCameraCenter();
	}
	return poses;
}
int System::GetNumberOfMap()
{
	return numOfMaps;
}
cv::Mat System::GetCameraCoordinates()
{
	unique_lock<mutex> lock2(mMutexState);
	
	return mpTracker->mCurrentFrame.GetCameraCenter();
}
cv::Mat System::GetCameraRotate()
{
	unique_lock<mutex> lock2(mMutexState);
	return mpTracker->mCurrentFrame.GetRotationInverse();

}
bool System::TrackingState()
{
	unique_lock<mutex> lock2(mMutexState);
	return mpTracker->mbOnlyTracking;
}
void System::ActivateLocalizationMode()
{
    unique_lock<mutex> lock(mMutexMode);
    mbActivateLocalizationMode = true;
}

void System::DeactivateLocalizationMode()
{
    unique_lock<mutex> lock(mMutexMode);
    mbDeactivateLocalizationMode = true;
}

bool System::MapChanged()
{
    static int n=0;
    int curn = mpMap->GetLastBigChangeIdx();
    if(n<curn)
    {
        n=curn;
        return true;
    }
    else
        return false;
}

void System::Reset()
{
    unique_lock<mutex> lock(mMutexReset);
    mbReset = true;
}

void System::Shutdown()
{
	mpLocalMapper->RequestFinish();
	mpLoopCloser->RequestFinish();
	if(mpViewer)
	{
		mpViewer->RequestFinish();
		while(!mpViewer->isFinished())
		{
			std::this_thread::sleep_for(std::chrono::microseconds(5000));
		}
	}

	// Wait until all thread have effectively stopped
	while(!mpLocalMapper->isFinished() || !mpLoopCloser->isFinished() || mpLoopCloser->isRunningGBA())
	{
		std::this_thread::sleep_for(std::chrono::microseconds(5000));
	}
	if(mpViewer)
		pangolin::BindToContext("ORB-SLAM2: Map Viewer");
	if (is_save_map){

		if(!mpTracker->mbOnlyTracking){
			if(!mpMap->GetAllKeyFrames().empty()){
				SaveMap(fileName);
				sprintf(fileTrajectory,"%iKeyFrameTrajectory.txt",numOfMaps);
				SaveKeyFrameTrajectoryTUM(fileTrajectory);
			}
		}
	}
}


void System::SaveTrajectoryTUM(const string &filename)
{
    cout << endl << "Saving camera trajectory to " << filename << " ..." << endl;
    if(mSensor==MONOCULAR)
    {
        cerr << "ERROR: SaveTrajectoryTUM cannot be used for monocular." << endl;
        return;
    }

    vector<KeyFrame*> vpKFs = mpMap->GetAllKeyFrames();
    sort(vpKFs.begin(),vpKFs.end(),KeyFrame::lId);

    // Transform all keyframes so that the first keyframe is at the origin.
    // After a loop closure the first keyframe might not be at the origin.
    cv::Mat Two = vpKFs[0]->GetPoseInverse();

    ofstream f;
    f.open(filename.c_str());
    f << fixed;

    // Frame pose is stored relative to its reference keyframe (which is optimized by BA and pose graph).
    // We need to get first the keyframe pose and then concatenate the relative transformation.
    // Frames not localized (tracking failure) are not saved.

    // For each frame we have a reference keyframe (lRit), the timestamp (lT) and a flag
    // which is true when tracking failed (lbL).
    list<ORB_SLAM2::KeyFrame*>::iterator lRit = mpTracker->mlpReferences.begin();
    list<double>::iterator lT = mpTracker->mlFrameTimes.begin();
    list<bool>::iterator lbL = mpTracker->mlbLost.begin();
    for(list<cv::Mat>::iterator lit=mpTracker->mlRelativeFramePoses.begin(),
        lend=mpTracker->mlRelativeFramePoses.end();lit!=lend;lit++, lRit++, lT++, lbL++)
    {
        if(*lbL)
            continue;

        KeyFrame* pKF = *lRit;

        cv::Mat Trw = cv::Mat::eye(4,4,CV_32F);

        // If the reference keyframe was culled, traverse the spanning tree to get a suitable keyframe.
        while(pKF->isBad())
        {
            Trw = Trw*pKF->mTcp;
            pKF = pKF->GetParent();
        }

        Trw = Trw*pKF->GetPose()*Two;

        cv::Mat Tcw = (*lit)*Trw;
        cv::Mat Rwc = Tcw.rowRange(0,3).colRange(0,3).t();
        cv::Mat twc = -Rwc*Tcw.rowRange(0,3).col(3);

        vector<float> q = Converter::toQuaternion(Rwc);

        f << setprecision(6) << *lT << " " <<  setprecision(9) << twc.at<float>(0) << " " << twc.at<float>(1) << " " << twc.at<float>(2) << " " << q[0] << " " << q[1] << " " << q[2] << " " << q[3] << endl;
    }
    f.close();
    cout << endl << "trajectory saved!" << endl;
}


void System::SaveKeyFrameTrajectoryTUM(const string &filename)
{
    cout << endl << "Saving keyframe trajectory to " << filename << " ..." << endl;

    vector<KeyFrame*> vpKFs = mpMap->GetAllKeyFrames();
    sort(vpKFs.begin(),vpKFs.end(),KeyFrame::lId);
    
    // Transform all keyframes so that the first keyframe is at the origin.
    // After a loop closure the first keyframe might not be at the origin.
    //cv::Mat Two = vpKFs[0]->GetPoseInverse();
    char keyF[100];
    ofstream f;
    f.open(filename.c_str());
    f << fixed;
    sprintf(keyF,"%iKeyFrameTrajectory.yaml",numOfMaps);
    cv::FileStorage fsp(keyF, cv::FileStorage::WRITE); 

    for(unsigned int i=0; i<vpKFs.size(); i++)
    {
        KeyFrame* pKF = vpKFs[i];

       // pKF->SetPose(pKF->GetPose()*Two);

        if(pKF->isBad())
            continue;
        cv::Mat R = pKF->GetRotation().t();
        vector<float> q = Converter::toQuaternion(R);
        cv::Mat t = pKF->GetCameraCenter();
	char keyN[100];
 	sprintf(keyN,"KeyFrame%i",i);   	
	write(fsp,keyN,pKF->GetCameraCenter());
        f << setprecision(6) << pKF->mTimeStamp << setprecision(7) << " " << t.at<float>(0) << " " << t.at<float>(1) << " " << t.at<float>(2)
          << " " << q[0] << " " << q[1] << " " << q[2] << " " << q[3] << endl;

    }
    int num=vpKFs.size();
    write(fsp,"NumberOfKeyFrames", num);
    fsp.release(); 	
    f.close();
    cout << endl << "trajectory saved!" << endl;
}

void System::SaveTrajectoryKITTI(const string &filename)
{
    cout << endl << "Saving camera trajectory to " << filename << " ..." << endl;
    if(mSensor==MONOCULAR)
    {
        cerr << "ERROR: SaveTrajectoryKITTI cannot be used for monocular." << endl;
        return;
    }

    vector<KeyFrame*> vpKFs = mpMap->GetAllKeyFrames();
    sort(vpKFs.begin(),vpKFs.end(),KeyFrame::lId);

    // Transform all keyframes so that the first keyframe is at the origin.
    // After a loop closure the first keyframe might not be at the origin.
    cv::Mat Two = vpKFs[0]->GetPoseInverse();

    ofstream f;
    f.open(filename.c_str());
    f << fixed;

    // Frame pose is stored relative to its reference keyframe (which is optimized by BA and pose graph).
    // We need to get first the keyframe pose and then concatenate the relative transformation.
    // Frames not localized (tracking failure) are not saved.

    // For each frame we have a reference keyframe (lRit), the timestamp (lT) and a flag
    // which is true when tracking failed (lbL).
    list<ORB_SLAM2::KeyFrame*>::iterator lRit = mpTracker->mlpReferences.begin();
    list<double>::iterator lT = mpTracker->mlFrameTimes.begin();
    for(list<cv::Mat>::iterator lit=mpTracker->mlRelativeFramePoses.begin(), lend=mpTracker->mlRelativeFramePoses.end();lit!=lend;lit++, lRit++, lT++)
    {
        ORB_SLAM2::KeyFrame* pKF = *lRit;

        cv::Mat Trw = cv::Mat::eye(4,4,CV_32F);

        while(pKF->isBad())
        {
          //  cout << "bad parent" << endl;
            Trw = Trw*pKF->mTcp;
            pKF = pKF->GetParent();
        }

        Trw = Trw*pKF->GetPose()*Two;

        cv::Mat Tcw = (*lit)*Trw;
        cv::Mat Rwc = Tcw.rowRange(0,3).colRange(0,3).t();
        cv::Mat twc = -Rwc*Tcw.rowRange(0,3).col(3);

        f << setprecision(9) << Rwc.at<float>(0,0) << " " << Rwc.at<float>(0,1)  << " " << Rwc.at<float>(0,2) << " "  << twc.at<float>(0) << " " <<
             Rwc.at<float>(1,0) << " " << Rwc.at<float>(1,1)  << " " << Rwc.at<float>(1,2) << " "  << twc.at<float>(1) << " " <<
             Rwc.at<float>(2,0) << " " << Rwc.at<float>(2,1)  << " " << Rwc.at<float>(2,2) << " "  << twc.at<float>(2) << endl;
    }
    f.close();
    cout << endl << "trajectory saved!" << endl;
}

int System::GetTrackingState()
{
    unique_lock<mutex> lock(mMutexState);
    return mTrackingState;
}

vector<MapPoint*> System::GetTrackedMapPoints()
{
    unique_lock<mutex> lock(mMutexState);
    return mTrackedMapPoints;
}

vector<cv::KeyPoint> System::GetTrackedKeyPointsUn()
{
    unique_lock<mutex> lock(mMutexState);
    return mTrackedKeyPointsUn;
}

void System::SaveMap(const string &filename)
{
    std::ofstream out(filename, std::ios_base::binary);
    if (!out)
    {
        cerr << "Cannot Write to Mapfile: " << fileName << std::endl;
        exit(-1);
    }
    cout << "Saving Mapfile: " << fileName << std::flush;
    boost::archive::binary_oarchive oa(out, boost::archive::no_header);
    oa << mpMap;
    oa << mpKeyFrameDatabase;
    cout << " ...done" << std::endl;
    out.close();
}

bool System::LoadMap(const string &filename)
{
    std::ifstream in(filename, std::ios_base::binary);
    if (!in)
    {
        cerr << "Cannot Open Mapfile: " << mapfile << " , Create a new one" << std::endl;
        return false;
    }
    cout << "Loading Mapfile: " << fileName << std::flush;
    boost::archive::binary_iarchive ia(in, boost::archive::no_header);
    ia >> mpMap;
    ia >> mpKeyFrameDatabase;
    mpKeyFrameDatabase->SetORBvocabulary(mpVocabulary);
    cout << " ...done" << std::endl;
    cout << "Map Reconstructing" << flush << endl;
    vector<ORB_SLAM2::KeyFrame*> vpKFS = mpMap->GetAllKeyFrames();
    cv::Mat last;
    sort(vpKFS.begin(),vpKFS.end(),KeyFrame::lId);
    int index = vpKFS.size();
    last=vpKFS[index-1]->GetPose();
    cout << "Mapa " << last << endl;
    distLoad=0;
    distLoad = sqrt(pow(last.at<float>(0,3),2)+pow(last.at<float>(1,3),2)+pow(last.at<float>(2,3),2));
    cout << "Length of Map " << distLoad << endl; 
    unsigned long mnFrameId = 0;
    for (auto it:vpKFS) {
        it->SetORBvocabulary(mpVocabulary);
        it->ComputeBoW();
        if (it->mnFrameId > mnFrameId)
            mnFrameId = it->mnFrameId;
    }
    Frame::nNextId = mnFrameId;
    cout << " ...done" << endl;
    in.close();
    return true;
}

} //namespace ORB_SLAM
