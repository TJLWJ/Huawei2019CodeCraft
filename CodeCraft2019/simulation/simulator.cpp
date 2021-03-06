#include "simulator.h"
#include "scheduler.h"
#include "assert.h"
#include "log.h"
#include <algorithm>

Simulator Simulator::Instance;

Simulator::Simulator()
    : m_scheduler(0), m_scheduledCarsN(0), m_reachedCarsN(0), m_conflictFlag(false)
    , m_isEnableCheater(true)
{ }

void Simulator::SetScheduler(Scheduler* scheduler)
{
    m_scheduler = scheduler;
}

void Simulator::SetEnableCheater(const bool& enable)
{
    Instance.m_isEnableCheater = enable;
}

void Simulator::HandleUpdateState(const SimCar::SimState& state)
{
    m_conflictFlag = false;
    if(state == SimCar::SCHEDULED)
        ++m_scheduledCarsN;
}

void Simulator::NotifyFirstPriority(const int& time, SimScenario& scenario, SimCar* car) const
{
    if (!car->GetIsLockOnNextRoad())
    {
        int length = std::min(car->GetCurrentRoad()->GetLimit(), car->GetCar()->GetMaxSpeed());
        if (car->GetCurrentPosition() + length <= car->GetCurrentRoad()->GetLength()) //can not pass
            return;
        if (m_scheduler != 0)
            m_scheduler->HandleBecomeFirstPriority(time, scenario, car);
        car->LockOnNextRoad(time);
    }
}

void Simulator::NotifyScheduleStart()
{
    m_reachedCarsN = 0;
    m_scheduledCarsN = 0;
}

void Simulator::NotifyScheduleCycleStart()
{
    m_conflictFlag = true;
}

bool Simulator::GetIsDeadLock(SimScenario& scenario) const
{
    return m_conflictFlag && scenario.GetOnRoadCarsN() > 0;
}

bool Simulator::GetIsCompleted(SimScenario& scenario) const
{
    return (m_scheduledCarsN - m_reachedCarsN) == scenario.GetOnRoadCarsN();
}

int Simulator::GetPositionInNextRoad(const int& time, SimScenario& scenario, SimCar* car) const
{
    int currentLimit = std::min(car->GetCurrentRoad()->GetLimit(), car->GetCar()->GetMaxSpeed());
    int s1 = std::min(currentLimit, car->GetCurrentRoad()->GetLength() - car->GetCurrentPosition());
    if (car->GetCurrentPosition() + currentLimit <= car->GetCurrentRoad()->GetLength()) //can not reach the next road
        return 0;
    int maxS2 = car->GetCar()->GetMaxSpeed() - s1;
    if (car->GetCurrentCross()->GetId() == car->GetCar()->GetToCrossId() && car->GetNextRoadId() < 0) //reach goal
        return maxS2;
    NotifyFirstPriority(time, scenario, car);
    ASSERT(car->GetNextRoadId() >= 0);
    int nextLimit = std::min(car->GetCar()->GetMaxSpeed(), Scenario::Roads()[car->GetNextRoadId()]->GetLimit());
    int s2 = std::min(maxS2, nextLimit - s1);
    if (s2 <= 0)
        return 0;
    return s2;
}

//car on first priority
SimCar* Simulator::PeekFirstPriorityCarOnRoad(const int& time, SimScenario& scenario, const SimRoad* road, const int& crossId) const
{
    SimCar* ret = 0;
    int position = road->GetRoad()->GetLength() + 1;
    bool isVip = false;
    for (int i = 1; i <= road->GetRoad()->GetLanes(); ++i)
    {
        auto& list = road->GetCarsTo(i, crossId);
        if (list.size() > 0)
        {
            SimCar* car = scenario.Cars()[(*list.begin())->GetId()];
            if (car->GetSimState(time) != SimCar::SCHEDULED && !car->GetIsIgnored())
            {
                int limit = std::min(car->GetCar()->GetMaxSpeed(), road->GetRoad()->GetLimit());
                ASSERT(car->GetCurrentPosition() + limit > road->GetRoad()->GetLength());//first priority
                if (ret == 0 || ((car->GetCurrentPosition() > position && car->GetCar()->GetIsVip() == isVip) ||
                    (car->GetCar()->GetIsVip() && !isVip))) //vip power
                {
                    if (ret != 0)
                        ret->UpdateWaiting(time, car);
                    ret = car;
                    position = car->GetCurrentPosition();
                    ASSERT(isVip == car->GetCar()->GetIsVip() || !isVip);
                    isVip = car->GetCar()->GetIsVip();
                }
                else
                    car->UpdateWaiting(time, ret);
            }
        }
    }
    if (ret != 0)
    {
        GetPositionInNextRoad(time, scenario, ret);//for notify first priority if needed
        ASSERT(ret->GetNextRoadId() >= 0 || ret->GetCurrentCross() == ret->GetCar()->GetToCross());
    }
    return ret;
}

bool CompareCarsPriority(SimCar* a, SimCar* b)
{
    ASSERT(a->GetCurrentCross() == b->GetCurrentCross());
    auto dirA = a->GetCurrentTurnType();
    auto dirB = b->GetCurrentTurnType();
    ASSERT(dirA != dirB);
    if (a->GetCar()->GetIsVip() != b->GetCar()->GetIsVip())
        return a->GetCar()->GetIsVip();
    if (dirA == Cross::DIRECT || dirB == Cross::DIRECT)
        return dirA == Cross::DIRECT;
    return dirA == Cross::LEFT;
}

//pass cross or just forward, return [true] means scheduled; [false] means waiting, require the car is the first one on its lane
bool Simulator::PassCrossOrJustForward(const int& time, SimScenario& scenario, SimCar* car)
{
    SimRoad* road = scenario.Roads()[car->GetCurrentRoad()->GetId()];
    ///logic moved
    Cross* cross = car->GetCurrentCross();
    auto& carlist = road->GetCarsTo(car->GetCurrentLane(), cross->GetId());
    ASSERT(car->GetCar() == *carlist.begin());
    int s2 = GetPositionInNextRoad(time, scenario, car);
    int nextRoadId = car->GetNextRoadId();
    bool reachGoal = nextRoadId < 0;
    if (reachGoal)
    {
        nextRoadId = cross->GetTurnDestinationId(road->GetRoad()->GetId(), Cross::DIRECT);
        ASSERT(s2 > 0);
        ASSERT(car->GetCar()->GetToCross() == cross);
    }
    
    //try pass cross
    if (nextRoadId >= 0)
    {
        static std::vector<SimCar*> priority;
        priority.clear();
        priority.reserve(4);
        DirectionType_Foreach(dir,
            Road* tmpRoad = cross->GetRoad(dir);
            if (tmpRoad != 0 && tmpRoad->GetId() != road->GetRoad()->GetId() && tmpRoad->GetId() != nextRoadId && tmpRoad->CanReachTo(cross->GetId()))
            {
                SimCar* car = tmpRoad->IsFromOrTo(cross->GetId()) ? m_firstPriorities[tmpRoad->GetId()].second : m_firstPriorities[tmpRoad->GetId()].first; //Simulator::PeekFirstPriorityCarOnRoad(time, scenario, scenario.Roads()[tmpRoad->GetId()], cross->GetId());
                if (car != 0 && (car->GetNextRoadId() == nextRoadId || (car->GetCurrentTurnType() == Cross::DIRECT && car->GetCar()->GetToCross() == cross && cross->GetTurnDirection(car->GetCurrentRoad()->GetId(), nextRoadId) == Cross::DIRECT)))
                    priority.push_back(car);
            }
        );
        priority.push_back(car);
        std::sort(priority.begin(), priority.end(), &CompareCarsPriority);
        if (priority[0] != car)
        {
            car->UpdateWaiting(time, priority[0]);
            return false;
        }
    }
    else
    {
        ASSERT(reachGoal);
    }

    if (reachGoal)
    {
        road->RunOut(car->GetCurrentLane(), !car->GetCurrentDirection());
        car->UpdateReachGoal(time);
        ++m_reachedCarsN;
        return true;
    }

    SimRoad* nextRoad = scenario.Roads()[nextRoadId];
    bool isFromOrTo = nextRoad->GetRoad()->IsFromOrTo(cross->GetId());
    int nextPosition = GetPositionInNextRoad(time, scenario, car);
    if (nextPosition <= 0) //just forward
    {
        //ASSERT(false);//the car which can not pass the cross should forward in another function
        int maxLength = std::min(car->GetCar()->GetMaxSpeed(), road->GetRoad()->GetLimit());
        int newPosition = std::min(car->GetCurrentPosition() + maxLength, road->GetRoad()->GetLength());
        ASSERT(newPosition == road->GetRoad()->GetLength());//the car which may pass the cross but can not pass it will stop at the head
        car->UpdatePosition(time, newPosition);
        return true;
    }

    //it's time to pass the cross!
    bool updatedState = false;
    for (int i = 1; i <= nextRoad->GetRoad()->GetLanes(); ++i)
    {
        auto& inlist = nextRoad->GetCarsFrom(i, cross->GetId());
        SimCar* lastcar = 0;
        if (inlist.size() > 0)
            lastcar = scenario.Cars()[(*--inlist.end())->GetId()];
        //need wait
        if (lastcar != 0 && lastcar->GetCurrentPosition() <= nextPosition && lastcar->GetSimState(time) != SimCar::SCHEDULED)
        {
            car->UpdateWaiting(time, lastcar);
            updatedState = true;
            break;
        }
        //pass the cross
        if (lastcar == 0 || lastcar->GetCurrentPosition() != 1)
        {
            //go on the new road & remove from old road
            nextRoad->RunIn(road->RunOut(car->GetCurrentLane(), !car->GetCurrentDirection()), i, !isFromOrTo);
            int newPosition = lastcar == 0 ? nextPosition : std::min(lastcar->GetCurrentPosition() - 1, nextPosition);
            car->UpdateOnRoad(time, nextRoad->GetRoad(), i, isFromOrTo, newPosition);
            updatedState = true;
            //break;
            return true;
        }
        //try next lane...
    }
    if (!updatedState) //this means no empty place for this car in next road
    {
        car->UpdatePosition(time, road->GetRoad()->GetLength()); //just move forward
        return true;
    }
    return false;
}

void UpdateCarsInLane(const int& time, SimScenario& scenario, SimRoad* &road, const int& lane, const bool& opposite, const bool& canBreak)
{
    auto& cars = road->GetCars(lane, opposite);
    SimCar* frontCar = 0;
    for (uint i = 0; i < cars.size(); ++i)
    {
        SimCar* car = scenario.Cars()[cars[i]->GetId()];
        SimCar::SimState state = car->GetSimState(time);
        if (car->GetIsIgnored())
            break;
        if (state != SimCar::SCHEDULED)
        {
            int speed = std::min(car->GetCurrentRoad()->GetLimit(), car->GetCar()->GetMaxSpeed());
            int nexPosition = car->GetCurrentPosition() + speed;
            if (i == 0) //the first car
            {
                ASSERT(frontCar == 0);
                if (nexPosition <= car->GetCurrentRoad()->GetLength()) //have no possible passing cross
                //if (Simulator::GetPositionInNextRoad(time, scenario, car) <= 0) //will not pass cross
                {
                    car->UpdatePosition(time, std::min(nexPosition, road->GetRoad()->GetLength()));
                }
                else //may pass cross
                {
                    if (canBreak)
                        break;
                }
            }
            else
            {
                ASSERT(frontCar != 0);
                int frontPosition = frontCar->GetCurrentPosition();
                if (frontCar->GetSimState(time) != SimCar::SCHEDULED && nexPosition >= frontPosition) //need wait
                {
                    car->UpdateWaiting(time, frontCar);
                    if (canBreak)
                        break;
                }
                else
                {
                    car->UpdatePosition(time, std::min(nexPosition, frontPosition - 1));
                }
            }
        }
        frontCar = car;
    }
}

void UpdateCarsInRoad(const int& time, SimScenario& scenario, SimRoad* road)
{
    int lanes = road->GetRoad()->GetLanes();
    for (int i = 0; i < lanes * 2; ++i)
    {
        bool opposite = i >= lanes;
        if (opposite && !road->GetRoad()->GetIsTwoWay())
            break;
        UpdateCarsInLane(time, scenario, road, (i % lanes) + 1, opposite, false);
    }
}

bool CompareRoadId(SimRoad* a, SimRoad* b)
{
    return a->GetRoad()->GetOriginId() < b->GetRoad()->GetOriginId();
}

Simulator::UpdateResult Simulator::Update(const int& time, SimScenario& scenario)
{
    SimCar::SetUpdateStateNotifier(Callback::Create(&Simulator::HandleUpdateState, this));
    Simulator::UpdateResult result;
    result.Conflict = false;

    NotifyScheduleStart();
    m_firstPriorities.resize(scenario.Roads().size());
    for (uint i = 0; i < scenario.Roads().size(); ++i)
    {
        SimRoad* road = scenario.Roads()[i];
        UpdateCarsInRoad(time, scenario, road);
        m_firstPriorities[i].first = PeekFirstPriorityCarOnRoad(time, scenario, road, road->GetRoad()->GetEndCrossId());
        m_firstPriorities[i].second = road->GetRoad()->GetIsTwoWay() ? PeekFirstPriorityCarOnRoad(time, scenario, road, road->GetRoad()->GetStartCrossId()) : 0;
    }
    InitializeCarsInGarage(time, scenario);
    GetVipOutFromGarage(time, scenario);
    while(true)
    {
        NotifyScheduleCycleStart();
        for (uint iCross = 0; iCross < Scenario::Crosses().size(); ++iCross)
        {
            Cross* cross = Scenario::Crosses()[iCross];
            int crossId = cross->GetId();
            static std::vector<SimRoad*> roads; //roads in this cross
            roads.clear();
            roads.reserve(4);
            DirectionType_Foreach(dir, 
                int id = cross->GetRoadId(dir);
                if (id >= 0)
                {
                    SimRoad* road = scenario.Roads()[id];
                    if (road->GetRoad()->GetEndCrossId() == crossId ||
                        (road->GetRoad()->GetStartCrossId() == crossId && road->GetRoad()->GetIsTwoWay()))
                        roads.push_back(road);
                }
            );
            std::sort(roads.begin(), roads.end(), &CompareRoadId);

            for (uint iRoad = 0; iRoad < roads.size(); ++iRoad)
            {
                SimRoad* road = roads[iRoad];
                //try pass cross (only the first priority can pass the cross)
                SimCar* firstPriority = 0;
                //while((firstPriority = Simulator::PeekFirstPriorityCarOnRoad(time, scenario, road, crossId)) != 0)
                while((firstPriority = road->GetRoad()->IsFromOrTo(cross->GetId()) ? m_firstPriorities[road->GetRoad()->GetId()].second : m_firstPriorities[road->GetRoad()->GetId()].first) != 0)
                {
                    int oldRoadId = firstPriority->GetCurrentRoad()->GetId();
                    int lane = firstPriority->GetCurrentLane();
                    bool opposite = !firstPriority->GetCurrentDirection();
                    if (!Simulator::PassCrossOrJustForward(time, scenario, firstPriority))
                    {
                        ASSERT (firstPriority->GetSimState(time) == SimCar::WAITING && firstPriority->GetWaitingCar(time) != 0);
                        break;
                    }
                    //UpdateCarsInRoad(time, scenario, road);
                    UpdateCarsInLane(time, scenario, road, lane, opposite, true);
                    (road->GetRoad()->IsFromOrTo(crossId) ? m_firstPriorities[road->GetRoad()->GetId()].second : m_firstPriorities[road->GetRoad()->GetId()].first)
                        = PeekFirstPriorityCarOnRoad(time, scenario, road, cross->GetId());
                    //GetVipOutFromGarage(time, scenario);
                    GetVipOutFromGarage(time, scenario, road->GetRoad()->GetPeerCross(cross)->GetId(), road->GetRoad()->GetId());
                    //GetVipOutFromGarage(time, scenario, crossId, firstPriority->GetCurrentRoad()->GetId());
                    //crossConflict = false;
                }
            }
        }
        if (GetIsCompleted(scenario)) //complete
        {
            result.Conflict = false;
            break;
        }
        if (GetIsDeadLock(scenario)) //conflict
        {
            PrintDeadLock(time, scenario);
            result.Conflict = true;
            break;
        }
    }
    if (!result.Conflict)
    {
        if (m_scheduler != 0)
            m_scheduler->HandleBeforeGarageDispatch(time, scenario);
        GetVipOutFromGarage(time, scenario);
        GetOutFromGarage(time, scenario);
    }
    //ASSERT(result.Conflict || scheduledCarsN - reachedCarsN == scenario.GetOnRoadCarsN());
    LOG("@ " << time << " Cars in garage : "<< scenario.GetCarInGarageN() << ", Cars on road : " << scenario.GetOnRoadCarsN() << ", Cars reached goal : " << scenario.GetReachCarsN());
    return result;
}

std::pair<int, int> Simulator::CanCarGetOutFromGarage(const int& time, SimScenario& scenario, SimCar* car) const
{
    int roadId = car->GetNextRoadId();
    ASSERT(roadId >= 0);
    SimRoad* road = scenario.Roads()[roadId];
    int maxLength = std::min(car->GetCar()->GetMaxSpeed(), road->GetRoad()->GetLimit());
    int crossId = car->GetCar()->GetFromCrossId();
    for (int i = 1; i <= road->GetRoad()->GetLanes(); ++i)
    {
        auto& cars = road->GetCarsFrom(i, crossId);
        if (cars.size() > 0)
        {
            SimCar* lastCar = scenario.Cars()[cars.back()->GetId()];
            ASSERT(car->GetCar()->GetIsVip() || lastCar->GetSimState(time) == SimCar::SCHEDULED);
            ASSERT(lastCar->GetCurrentPosition() > 0);
            if (lastCar->GetSimState(time) != SimCar::SCHEDULED && lastCar->GetCurrentPosition() <= maxLength)
                return std::make_pair(-1, -1); //need wait
            //try next lane
            if (lastCar->GetSimState(time) == SimCar::SCHEDULED && lastCar->GetCurrentPosition() == 1)
                continue;
            //decide real position
            maxLength = std::min(maxLength, lastCar->GetCurrentPosition() - 1);
        }
        return std::make_pair(i, maxLength);
        break;
    }
    return std::make_pair(0, 0); //no empty place
}

bool Simulator::GetCarOutFromGarage(const int& time, SimScenario& scenario, SimCar* car) const
{
    auto canGoout = CanCarGetOutFromGarage(time, scenario, car);
    bool goout = canGoout.first > 0 && canGoout.second > 0;
    if (goout)
    {
        int roadId = car->GetNextRoadId();
        ASSERT(roadId >= 0);
        SimRoad* road = scenario.Roads()[roadId];
        bool isFromOrTo = road->GetRoad()->IsFromOrTo(car->GetCar()->GetFromCrossId());
        car->UpdateOnRoad(time, road->GetRoad(), canGoout.first, isFromOrTo, canGoout.second);
        road->RunIn(car->GetCar(), canGoout.first, !isFromOrTo);
    }
    if (canGoout.first >= 0 && canGoout.second >= 0 && (!car->GetCar()->GetIsPreset() || car->GetCanChangeRealTime()) && m_isEnableCheater)
        car->SetRealTime(time + (goout ? 0 : 1));
    return goout;
}

struct CompareCarsInGarageStruct
{
    bool operator() (SimCar* a, SimCar* b)
    {
        //only compare VIP or non-VIP cars
        if (a == 0 || b == 0)
            return a != 0;
        ASSERT(a->GetCar()->GetIsVip() == b->GetCar()->GetIsVip());
        if (a->GetRealTime() != b->GetRealTime())
            return a->GetRealTime() < b->GetRealTime();
        return a->GetCar()->GetOriginId() < b->GetCar()->GetOriginId();
    }
} CompareCarsInGarage;

/* non-VIP cars */
void Simulator::GetOutFromGarage(const int& time, SimScenario& scenario) const
{
    int getOutCounter = 0;
    for (uint i = 0; i < scenario.Garages().size(); ++i)
    {
        auto& cangoCars = m_carsInGarage[i];
        for (uint iIndex = 0; iIndex < cangoCars.size(); ++iIndex)
        {
            SimCar* car = cangoCars[iIndex];
            int oriTime = car->GetRealTime();
            if (m_scheduler != 0
                && (!car->GetCar()->GetIsPreset() || car->GetCanChangePath() || car->GetCanChangeRealTime())
                && !car->GetCar()->GetIsVip()) //vip car is not expect get out in function
                m_scheduler->HandleGetoutGarage(time, scenario, car);
            ASSERT(car->GetRealTime() == oriTime || car->GetRealTime() > time);
            if (car->GetRealTime() > time)
                continue;

            /* it's time to go! */
            bool goout = GetCarOutFromGarage(time, scenario, car);
            ASSERT(!goout || !car->GetCar()->GetIsVip()); //vip car is not expect get out in function
            if (!goout)
            {
                if (car->GetCar()->GetIsPreset() && !car->GetCanChangeRealTime() && !car->GetIsLockOnNextRoad())
                    car->LockOnNextRoad(time);
                car->UpdateStayInGarage(time);
            }
            else //go out
            {
                ++getOutCounter;
            }
        }
    }
    if (getOutCounter > 0)
        LOG("@" << time << " number of non-VIP cars get on road form garage : " << getOutCounter);
}

void Simulator::InitializeCarsInGarage(const int& time, SimScenario& scenario)
{
    //m_vipCarsInGarage.clear();
    if (m_carsInGarage.size() != scenario.Garages().size())
    {
        m_carsInGarage.resize(scenario.Garages().size());
        m_vipCarsInGarage.resize(scenario.Garages().size());
    }
    for (uint i = 0; i < scenario.Garages().size(); ++i)
    {
        m_carsInGarage[i].clear();
        m_vipCarsInGarage[i].clear();
    }
    for (uint i = 0; i < scenario.Cars().size(); ++i)
    {
        SimCar* car = scenario.Cars()[i];
        if (car == 0 || !car->GetIsInGarage() || car->GetRealTime() > time) continue;
        if (car->GetCar()->GetIsVip())
            m_vipCarsInGarage[car->GetCar()->GetFromCrossId()].push_back(car);
        else
            m_carsInGarage[car->GetCar()->GetFromCrossId()].push_back(car);
    }
    for (uint i = 0; i < scenario.Garages().size(); ++i)
    {
        std::sort(m_carsInGarage[i].begin(), m_carsInGarage[i].end(), CompareCarsInGarage);
        std::sort(m_vipCarsInGarage[i].begin(), m_vipCarsInGarage[i].end(), CompareCarsInGarage);
    }
}

void Simulator::GetVipOutFromGarage(const int& time, SimScenario& scenario, const int& crossId, const int& roadId)
{
    if (crossId >= 0)
    {
        auto& garage = m_vipCarsInGarage[crossId];
        for (uint i = 0; i < garage.size(); ++i)
        {
            SimCar* car = garage[i];
            if (car == 0) break; //continue;
            if (!car->GetIsInGarage() || car->GetRealTime() > time) continue;
            int oldTime = car->GetRealTime();
            if (m_scheduler != 0
                && (!car->GetCar()->GetIsPreset() || car->GetCanChangePath() || car->GetCanChangeRealTime()))
                m_scheduler->HandleGetoutGarage(time, scenario, car);
            if (car->GetRealTime() <= time)
            {
                ASSERT(car->GetNextRoadId() >= 0);
                ASSERT(car->GetRealTime() == oldTime || car->GetRealTime() > time);
                if (roadId < 0 || car->GetNextRoadId() == roadId)
                {
                    bool goout = GetCarOutFromGarage(time, scenario, car);
                    if (!goout)
                    {
                        if (car->GetCar()->GetIsPreset() && !car->GetCanChangeRealTime() && !car->GetIsLockOnNextRoad())
                            car->LockOnNextRoad(time);
                        car->UpdateStayInGarage(time);
                    }
                }
            }
        }
    }
    else
    {
        ASSERT(roadId < 0);
        for (uint i = 0; i < m_vipCarsInGarage.size(); ++i)
            GetVipOutFromGarage(time, scenario, i, roadId);
    }
}

std::list<SimCar*> Simulator::GetDeadLockCars(const int& time, SimScenario& scenario) const
{
    //find all waiting first priority cars
    std::list<SimCar*> allFirstPriorityCars;
    for (uint iCross = 0; iCross < Scenario::Crosses().size(); ++iCross)
    {
        Cross* cross = Scenario::Crosses()[iCross];
        int crossId = cross->GetId();
        for (int i = (int)Cross::NORTH; i <= (int)Cross::WEST; ++i)
        {
            int id = cross->GetRoadId((Cross::DirectionType)i);
            if (id >= 0)
            {
                SimRoad* road = scenario.Roads()[id];
                if (road->GetRoad()->CanReachTo(crossId))
                {
                    SimCar* firstPriority = PeekFirstPriorityCarOnRoad(time, scenario, road, crossId);
                    if (firstPriority != 0 && firstPriority->GetSimState(time) != SimCar::SCHEDULED)
                    {
                        ASSERT(firstPriority->GetSimState(time) == SimCar::WAITING);
                        ASSERT_MSG(firstPriority->GetIsLockOnNextRoad() || firstPriority->GetCurrentCross() == firstPriority->GetCar()->GetToCross()
                            , *(firstPriority->GetCar()) << " cross " << iCross << " road " << ID(*(firstPriority->GetCurrentRoad())) << " pos " << firstPriority->GetCurrentPosition());
                        //if (firstPriority->GetWaitingCar(time)->GetCurrentCross()->GetId() != crossId)
                            allFirstPriorityCars.push_back(firstPriority);
                    }
                }
            }
        }
    }

    //find a loop in them
    std::vector<SimCar*> firstPriorities;
    int deadLockIndex = -1;
    firstPriorities.reserve(allFirstPriorityCars.size());
    SimCar* currentCar = allFirstPriorityCars.front();
    while (true)
    {
        SimCar* newCar = currentCar->GetWaitingCar(time);
        if (currentCar->GetIsLockOnNextRoad()) //this is the first priority car
        {
            //try find it in recorded cars
            for (uint i = 0; i < firstPriorities.size(); ++i)
            {
                if (firstPriorities[i] == currentCar)
                {
                    LOG("find loop of " << ID(*(currentCar->GetCar())));
                    deadLockIndex = i;
                    break;
                }
            }
            //if finded break
            if (deadLockIndex >= 0)
                break;
            Cross* newCross = newCar->GetCurrentCross();
            Cross* currentCross = currentCar->GetCurrentCross();
            if (currentCar->GetIsLockOnNextRoad())
            {
                if (newCross != currentCross)
                {
                    LOG("find loop jump from " << ID(*currentCross)
                        << " to " << ID(*newCross));
                }
                firstPriorities.push_back(currentCar);
                LOG("push back " << ID(*(currentCar->GetCar())));
            }
        }
        currentCar = newCar;
    }

    std::list<SimCar*> result;
    for (uint i = deadLockIndex; i < firstPriorities.size(); ++i)
        result.push_back(firstPriorities[i]);
    return result;
}

void Simulator::PrintCrossState(const int& time, SimScenario& scenario, Cross* cross) const
{
    int crossId = cross->GetId();
    for (int i = (int)Cross::NORTH; i <= (int)Cross::WEST; ++i)
    {
        int id = cross->GetRoadId((Cross::DirectionType)i);
        if (id >= 0)
        {
            SimRoad* road = scenario.Roads()[id];
            if (road->GetRoad()->CanStartFrom(crossId))
            {
                LOG("Road [" << road->GetRoad()->GetId() << "] direction : " << i << " lanes " << road->GetRoad()->GetLanes() << " limit " << road->GetRoad()->GetLimit());
                for (int j = 1; j <= road->GetRoad()->GetLanes(); ++j)
                {
                    LOG(" \tLane " << j);
                    auto& cars = road->GetCarsFrom(j, crossId);
                    for (auto carIte = cars.begin(); carIte != cars.end(); ++carIte)
                    {
                        SimCar* car = scenario.Cars()[(*carIte)->GetId()];
                        if (car->GetSimState(time) == SimCar::SCHEDULED || GetPositionInNextRoad(time, scenario, car) <= 0)
                            break;
                        LOG(" \t\t" << *(car->GetCar())
                            << " pos " << car->GetCurrentPosition()
                            << " speed " << car->GetCar()->GetMaxSpeed()
                            //<< " dir " << cross->GetTurnDirection(road->GetRoad()->GetId(), car->GetNextRoadId())
                            );
                    }
                }
            }
        }
    }
}

void Simulator::PrintDeadLock(const int& time, SimScenario& scenario) const
{
    if (LOG_IS_ENABLE)
    {
        auto result = GetDeadLockCars(time, scenario);
        for (auto ite = result.begin(); ite != result.end(); ite++)
        {
            SimCar*& car = *ite;
            Cross* cross = car->GetCurrentCross();
            Road* road = car->GetCurrentRoad();
            LOG("the " << *(car->GetCar())
                << " cross " << cross->GetOriginId() << "(" << cross->GetId() << ")"
                << " road " << road->GetOriginId() << "(" << road->GetId() << ")"
                << " lane " << car->GetCurrentLane()
                << " position " << car->GetCurrentPosition()
                << " next " << "(" << car->GetNextRoadId() << ")"
                << " dir " << (car->GetNextRoadId() < 0 ? Cross::DIRECT : cross->GetTurnDirection(road->GetId(), car->GetNextRoadId()))
                << " waiting for " << *(car->GetWaitingCar(time)->GetCar())
                );
        }
    }
}