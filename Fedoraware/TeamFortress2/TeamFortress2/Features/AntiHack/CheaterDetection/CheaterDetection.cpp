#include "CheaterDetection.h"

bool CCheaterDetection::ShouldScan(){
	if (iLastScanTick == I::GlobalVars->tickcount) { return false; }
	
	if (flLastFrameTime) {
		const float flRealFrameTime = I::GlobalVars->realtime - flLastFrameTime;
		flLastFrameTime = I::GlobalVars->realtime;
		const int iRealFPS = static_cast<int>(1.0f / flRealFrameTime);

		if (iRealFPS < server.iTickRate)
		{ return false; }
	}

	if (const INetChannel* NetChannel = I::EngineClient->GetNetChannelInfo()) {
		const float flLastReceive = NetChannel->GetTimeSinceLastReceived();
		const float flMaxReceive = I::GlobalVars->interval_per_tick * 2;
		const bool bIsTimeOut = NetChannel->IsTimingOut();
		const bool bShouldSkip = (flLastReceive > flMaxReceive) || bIsTimeOut;
		if (bShouldSkip) { return false; }
	}

	return true;
}

bool CCheaterDetection::ShouldScanEntity(CBaseEntity* pEntity){
	const int iIndex = pEntity->GetIndex();

	// dont scan invalid players
	if (pEntity->IsAlive() || pEntity->IsAGhost() || pEntity->IsTaunting()) { return false; }

	// dont scan players that arent simulated this tick
	if (pEntity->GetSimulationTime() == pEntity->GetOldSimulationTime()) { return false; }

	// dont scan if we can't get playerinfo
	PlayerInfo_t pInfo{};
	if (!I::EngineClient->GetPlayerInfo(iIndex, &pInfo)) { return false; }
	
	// dont scan ignored players, friends, or players already marked as cheaters
	switch (G::PlayerPriority[pInfo.friendsID].Mode){
	case 0: case 1: case 4:
	{ return false; }
	}

	return true;
}

bool CCheaterDetection::IsPitchLegal(CBaseEntity* pSuspect){
	const Vec3 vAngles = pSuspect->GetEyeAngles();
	return fabsf(vAngles.x) < 90.f;
}

void CCheaterDetection::ReportTickCount(std::pair<CBaseEntity*, int> pReport){
	if (mData[pReport.first].pChoke.first = 0) { mData[pReport.first].pChoke.second = pReport.second; return; }
	mData[pReport.first].pChoke.first++;
	mData[pReport.first].pChoke.second+= pReport.second;
	return;
}

bool CCheaterDetection::CheckBHop(CBaseEntity* pEntity){
	const bool bOnGround = pEntity->OnSolid();	//	NOTE: groundentity isn't networked properly sometimes i think
	if (bOnGround) { mData[pEntity].pBhop.first++; }
	else if (mData[pEntity].pBhop.first == 1) { mData[pEntity].pBhop.second++; mData[pEntity].pBhop.first = 0; }
	else { mData[pEntity].pBhop = {0, 0}; }

	if (mData[pEntity].pBhop.second > INT_MAX_BHOP) { mData[pEntity].iPlayerSuspicion++; mData[pEntity].pBhop = {0, 0}; return true; }
	return false;
}

bool CCheaterDetection::AreAnglesSuspicious(CBaseEntity* pEntity){
	// first check if we should scan this player at all
	if (G::ChokeMap[pEntity->GetIndex()] > 0 || pEntity->GetVelocity().Length() < 10.f || mData[pEntity].vLastAngle.IsZero()) { mData[pEntity].pTrustAngles = {false, {0, 0}}; return false; } //	angles don't update the way I WANT them to if the player is not moving.
	if (!mData[pEntity].pTrustAngles.first){	//	we are not suspicious of this player yet
		const Vec3 vCurAngles = pEntity->GetEyeAngles();
		const Vec2 vDelta = Vec2{vCurAngles.x, vCurAngles.y} - mData[pEntity].vLastAngle;
		const float flDelta = vDelta.Length();

		if (flDelta > (20.f)) { 
			if (!mData[pEntity].bDidDamage) { mData[pEntity].pTrustAngles = {true, {vCurAngles.x, vCurAngles.y}}; }
			else { return true; }	//	20 degree flick followed by damage, very suspicious.
		}
	}
	else {
		//	check for noise on this player (how much their mouse moves after the initial flick)
		const Vec3 vCurAngles = pEntity->GetEyeAngles();
		const Vec2 vDelta = Vec2{vCurAngles.x, vCurAngles.y} - mData[pEntity].pTrustAngles.second;
		const float flDelta = vDelta.Length();

		if (flDelta < (5.f * server.flMultiplier)) { mData[pEntity].pTrustAngles = {false, {0, 0}}; return true; }
		else { mData[pEntity].pTrustAngles = {false, {0, 0}}; }
	}
	return false;
}

void CCheaterDetection::OnDormancy(CBaseEntity* pEntity){
	mData[pEntity].pTrustAngles = {false, {0, 0}};
	mData[pEntity].iNonDormantCleanQueries = 0;
	mData[pEntity].vLastAngle = {};
	mData[pEntity].pBhop = {false, 0};
	mFired[pEntity] = false;
}

void CCheaterDetection::OnTick()
{
	const auto pLocal = g_EntityCache.GetLocal();
	if (!pLocal || !I::EngineClient->IsConnected() || G::ShouldShift || !ShouldScan()) { return; }
	iLastScanTick = I::GlobalVars->tickcount;
	flScanningTime = I::GlobalVars->curtime - flFirstScanTime;
	FindScores();
	FindHitchances();

	for (int n = 1; n < I::EngineClient->GetMaxClients(); n++)
	{
		CBaseEntity* pEntity = I::ClientEntityList->GetClientEntity(n);
		if (!pEntity) { continue; }
		if (pEntity->GetDormant()) { OnDormancy(pEntity); continue; }
		if (!ShouldScanEntity(pEntity)) { OnDormancy(pEntity); continue; }
		if (pEntity == pLocal) { continue; }	//	i think for this code to run the local player has to be cheating anyway :thinking:
		bool bMarked = false;	//	used for suspicion reduction

		PlayerInfo_t pInfo{};
		if (!I::EngineClient->GetPlayerInfo(pEntity->GetIndex(), &pInfo)) { continue; }

		CalculateHitChance(pEntity);

		if (!IsPitchLegal(pEntity)) { bMarked = true; mData[pEntity].iPlayerSuspicion = INT_MAX_SUSPICION; Utils::ConLog("CheaterDetection", tfm::format("%s marked for OOB angles.", pInfo.name).c_str(), {224, 255, 131, 255}); }
		if (AreAnglesSuspicious(pEntity)) { bMarked = true; mData[pEntity].iPlayerSuspicion++; Utils::ConLog("CheaterDetection", tfm::format("%s infracted for suspicious angles.", pInfo.name).c_str(), {224, 255, 131, 255}); }
		if (CheckBHop(pEntity)) { bMarked = true; Utils::ConLog("CheaterDetection", tfm::format("%s infracted for bunny-hopping.", pInfo.name).c_str(), {224, 255, 131, 255}); }

		//analytical analysis
		if (mData[pEntity].pChoke.second && mData[pEntity].pChoke.first) { if (((float)mData[pEntity].pChoke.second / (float)mData[pEntity].pChoke.first) > (14.f / server.flMultiplier) && mData[pEntity].pChoke.first > 10) { mData[pEntity].iPlayerSuspicion++; Utils::ConLog("CheaterDetection", tfm::format("%s infracted for high avg packet choking {%.1f / %.1f}.", pInfo.name, mData[pEntity].pChoke.second, (14.f / server.flMultiplier)).c_str(), { 224, 255, 131, 255 }); mData[pEntity].pChoke = { 0, 0.f }; } }
		if (flScanningTime > 60.f){
			if (mData[pEntity].flHitchance && server.flHighAccuracy && mData[pEntity].mShots.first > 25) { if (mData[pEntity].flHitchance > (server.flHighAccuracy) && !mData[pEntity].pDetections.first) { mData[pEntity].iPlayerSuspicion += 5; Utils::ConLog("CheaterDetection", tfm::format("%s infracted for extremely high accuracy {%.1f / %.1f}.", pInfo.name, mData[pEntity].flHitchance, server.flHighAccuracy).c_str(), {224, 255, 131, 255}); } }
			if (mData[pEntity].flScorePerSecond && server.flAverageScorePerSecond) { if (mData[pEntity].flScorePerSecond > (std::max(server.flAverageScorePerSecond, server.flFloorScore) * 3) && !mData[pEntity].pDetections.second) { mData[pEntity].iPlayerSuspicion += 5; Utils::ConLog("CheaterDetection", tfm::format("%s infracted for extremely high score per second {%.1f / %.1f}.", pInfo.name, mData[pEntity].flScorePerSecond, server.flAverageScorePerSecond).c_str(), {224, 255, 131, 255}); } }
		}

		const Vec3 vAngles = pEntity->GetEyeAngles();
		mData[pEntity].vLastAngle = {vAngles.x, vAngles.y};
		if (mData[pEntity].iPlayerSuspicion > INT_MAX_SUSPICION) { mData[pEntity].iPlayerSuspicion = 0; G::PlayerPriority[pInfo.friendsID].Mode = 4; }
	}
}

void CCheaterDetection::FillServerInfo(){
	server.flAverageScorePerSecond = 0.f;
	server.iTickRate = 1.f / I::GlobalVars->interval_per_tick;
	server.flMultiplier = 66.7 / server.iTickRate;
	Utils::ConLog("CheaterDetection[UTIL]", tfm::format("Calculated server tickrate & created appropriate multiplier {%.1f | %.1f}.", server.iTickRate, server.flMultiplier).c_str(), {224, 255, 131, 255});
}

void CCheaterDetection::FindScores(){	//	runs every 30 seconds
	if (I::GlobalVars->curtime - flLastScoreTime < 30.f) { return; }
	CTFPlayerResource* cResource = g_EntityCache.GetPR();
	if (!cResource){ return; }
	flLastScoreTime = I::GlobalVars->curtime;
	
	float flTotalAvg = 0;
	int iTotalPlayers = 0;
	for (int n = 1; n < I::EngineClient->GetMaxClients(); n++)
	{
		CBaseEntity* pEntity = I::ClientEntityList->GetClientEntity(n);
		if (!pEntity || !cResource->GetValid(pEntity->GetIndex())) { continue; }
		const float iScore = cResource->GetScore(pEntity->GetIndex()) / cResource->GetConnectionTime(pEntity->GetIndex());
		flTotalAvg += iScore; iTotalPlayers++;	//	used for calculating the average score
	}

	if (!flTotalAvg || !iTotalPlayers) { return; }

	// now that we've gone through all players (including local) find the avg
	server.flAverageScorePerSecond = (flTotalAvg / (float)iTotalPlayers);
	Utils::ConLog("CheaterDetection[UTIL]", tfm::format("Calculated avg. server score per second at %.1f.", server.flAverageScorePerSecond).c_str(), {224, 255, 131, 255});
}

void CCheaterDetection::FindHitchances(){	//	runs every 30 seconds
	if (I::GlobalVars->curtime - flLastAccuracyTime < 30.f) { return; }
	flLastAccuracyTime = I::GlobalVars->curtime;

	const float flAvg = (float)server.iHits / (float)server.iMisses;
	server.flHighAccuracy = std::clamp(flAvg * 2, .05f, .95f);

	Utils::ConLog("CheaterDetection[UTIL]", tfm::format("Calculated server hitchance data {%.1f | %.1f}", flAvg, server.flHighAccuracy).c_str(), {224, 255, 131, 255});
}

void CCheaterDetection::Reset(){
	server = ServerData{};
	mData.clear();
	mFired.clear();
	iLastScanTick = 0;
	flLastFrameTime = 0.f;
	flFirstScanTime = 0.f;
	flScanningTime = 0.f;
	flLastScoreTime = 0.f;
	flLastAccuracyTime = 0.f;
}

void CCheaterDetection::OnLoad(){
	Reset();
	FillServerInfo();
	flLastAccuracyTime = I::GlobalVars->curtime; flLastScoreTime = I::GlobalVars->curtime;
}

void CCheaterDetection::CalculateHitChance(CBaseEntity* pEntity){
	mData[pEntity].flHitchance = (float)mData[pEntity].mShots.first / (float)mData[pEntity].mShots.second;
}

void CCheaterDetection::ReportShot(int iIndex){
	CBaseEntity* pEntity = I::ClientEntityList->GetClientEntity(iIndex);
	if (!pEntity){ return; }
	mData[pEntity].mShots.second++;
	server.iMisses++;
}

void CCheaterDetection::ReportDamage(CGameEvent* pEvent){
	const int iIndex = pEvent->GetInt("attacker");
	CBaseEntity* pEntity = I::ClientEntityList->GetClientEntity(iIndex);
	if (!pEntity || pEntity->GetDormant()){ return; }
	CBaseCombatWeapon* pWeapon = pEntity->GetActiveWeapon();
	if (!pWeapon || Utils::GetWeaponType(pWeapon) != EWeaponType::HITSCAN) { return; }
	if (I::GlobalVars->tickcount - mData[pEntity].iLastDamageEventTick <= 1){ return; }
	mData[pEntity].iLastDamageEventTick = I::GlobalVars->tickcount;
	mData[pEntity].mShots.first++; mData[pEntity].bDidDamage = true;
	server.iHits++;
}