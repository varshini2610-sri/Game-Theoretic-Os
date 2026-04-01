#include <iostream>
#include <vector>
#include <queue>
#include <algorithm>
#include <limits>
#include <numeric>
#include <iomanip>
#include <cmath>
#include <map>
#include <random>
#include <string>

using namespace std;

// Strategy multiplier bounds: reported = true * multiplier
constexpr double MIN_MULT  = 0.1;
constexpr double MAX_MULT  = 3.0;
constexpr double MULT_STEP = 0.1;

// ─────────────────────────────────────────────
//  Incentive Weight
//  Long-burst processes gain more from lying,
//  so they get a higher incentive weight [0,1].
// ─────────────────────────────────────────────

double incentiveWeight(int trueBurst, int maxBurst)
{
    if (maxBurst <= 0) return 0.0;
    return static_cast<double>(trueBurst) / maxBurst;
}

// ─────────────────────────────────────────────
//  Normalised Slowdown Utility
//  slowdown = (wait + burst) / burst
//  utility  = -slowdown  (higher is better)
//  Fairer than raw wait — short jobs aren't
//  disproportionately rewarded.
// ─────────────────────────────────────────────

double slowdown(int wait, int burst)
{
    if (burst <= 0) return 0.0;
    return static_cast<double>(wait + burst) / burst;
}

double slowdownUtility(int wait, int burst)
{
    return -slowdown(wait, burst);
}

// ─────────────────────────────────────────────
//  Process
// ─────────────────────────────────────────────

struct Process
{
    int    id;
    int    arrival;
    int    trueService;
    double multiplier;

    int    reportedService;
    int    remaining;
    int    finish;
    int    waiting;
    int    turnaround;
    double vcgPenalty;

    Process(int i, int a, int s, double mult = 1.0)
        : id(i), arrival(a), trueService(s), multiplier(mult),
          reportedService(0), remaining(0), finish(0),
          waiting(0), turnaround(0), vcgPenalty(0.0)
    { applyStrategy(); }

    void applyStrategy()
    {
        reportedService = max(1,
            static_cast<int>(round(trueService * multiplier)));
        remaining  = trueService;
        finish     = waiting = turnaround = 0;
        vcgPenalty = 0.0;
    }

    double utility() const
    {
        return slowdownUtility(waiting, trueService) - vcgPenalty;
    }
};

// ─────────────────────────────────────────────
//  Metrics
// ─────────────────────────────────────────────

struct Metrics
{
    double avgWaiting;
    double avgSlowdown;
    double throughput;
    double fairness;
    Metrics() : avgWaiting(0.0), avgSlowdown(0.0), throughput(0.0), fairness(0.0) {}
    Metrics(double w, double s, double t, double f)
        : avgWaiting(w), avgSlowdown(s), throughput(t), fairness(f) {}
};

double jainFairness(const vector<double>& v)
{
    if (v.empty()) return 1.0;
    double sum = 0.0, sq = 0.0;
    for (double x : v) { sum += x; sq += x * x; }
    if (sq == 0.0) return 1.0;
    return (sum * sum) / ((double)v.size() * sq);
}

Metrics computeMetrics(const vector<Process>& p, int totalTime)
{
    double avgW = 0.0, avgS = 0.0;
    vector<double> slows;
    for (const auto& x : p)
    {
        avgW += x.waiting;
        double s = slowdown(x.waiting, x.trueService);
        avgS += s;
        slows.push_back(s);
    }
    int n = (int)p.size();
    return Metrics(avgW/n, avgS/n, (double)n/totalTime, jainFairness(slows));
}

// ─────────────────────────────────────────────
//  Schedulers
// ─────────────────────────────────────────────

Metrics runFCFS(vector<Process> p)
{
    sort(p.begin(), p.end(),
         [](const Process& a, const Process& b){ return a.arrival < b.arrival; });
    int time = 0;
    for (int i = 0; i < (int)p.size(); i++)
    {
        time            = max(time, p[i].arrival);
        p[i].waiting    = time - p[i].arrival;
        time           += p[i].trueService;
        p[i].finish     = time;
        p[i].turnaround = p[i].finish - p[i].arrival;
    }
    return computeMetrics(p, time);
}

Metrics runRR(vector<Process> p, int quantum)
{
    int n = (int)p.size(), time = 0, done = 0;
    queue<int> q;
    vector<bool> inQ(n, false);
    auto enq = [&](){
        for (int i = 0; i < n; i++)
            if (!inQ[i] && p[i].arrival <= time) { q.push(i); inQ[i] = true; }
    };
    enq();
    while (done < n)
    {
        if (q.empty()) { time++; enq(); continue; }
        int i = q.front(); q.pop();
        int e = min(quantum, p[i].remaining);
        p[i].remaining -= e; time += e; enq();
        if (p[i].remaining == 0)
        {
            p[i].finish     = time;
            p[i].turnaround = time - p[i].arrival;
            p[i].waiting    = p[i].turnaround - p[i].trueService;
            done++;
        }
        else q.push(i);
    }
    return computeMetrics(p, time);
}

Metrics runSRT(vector<Process> p)
{
    int n = (int)p.size(), time = 0, done = 0;
    typedef tuple<int,int,int> T;
    priority_queue<T, vector<T>, greater<T> > heap;
    sort(p.begin(), p.end(),
         [](const Process& a, const Process& b){ return a.arrival < b.arrival; });
    int next = 0;
    while (done < n)
    {
        while (next < n && p[next].arrival <= time)
        {
            heap.push(make_tuple(p[next].remaining, p[next].arrival, next));
            next++;
        }
        if (heap.empty()) { if (next < n) time = p[next].arrival; continue; }
        T top = heap.top(); heap.pop();
        int rem = get<0>(top);
        int idx = get<2>(top);
        if (p[idx].remaining == 0) continue;
        if (p[idx].remaining != rem)
        {
            heap.push(make_tuple(p[idx].remaining, p[idx].arrival, idx));
            continue;
        }
        p[idx].remaining--; time++;
        if (p[idx].remaining == 0)
        {
            done++;
            p[idx].finish     = time;
            p[idx].turnaround = time - p[idx].arrival;
            p[idx].waiting    = p[idx].turnaround - p[idx].trueService;
        }
        else heap.push(make_tuple(p[idx].remaining, p[idx].arrival, idx));
    }
    return computeMetrics(p, time);
}

// ─────────────────────────────────────────────
//  VCG Penalty
//  Charges each underreporting process for the
//  extra wait it imposed on all other processes.
//  Penalty = sum of harm to others / own burst.
// ─────────────────────────────────────────────

void applyVCGPenalties(vector<Process>& p)
{
    int n = (int)p.size();
    for (int i = 0; i < n; i++)
    {
        if (p[i].multiplier >= 1.0 - 1e-9) { p[i].vcgPenalty = 0.0; continue; }

        vector<Process> cf = p;
        cf[i].multiplier = 1.0;
        cf[i].applyStrategy();

        int time = 0, done = 0;
        vector<bool> fin(n, false);
        vector<int>  cfW(n, 0);
        while (done < n)
        {
            int idx = -1, best = numeric_limits<int>::max();
            for (int j = 0; j < n; j++)
                if (!fin[j] && cf[j].arrival <= time && cf[j].reportedService < best)
                    { best = cf[j].reportedService; idx = j; }
            if (idx == -1) { time++; continue; }
            cfW[idx] = time - cf[idx].arrival;
            time    += cf[idx].trueService;
            fin[idx] = true; done++;
        }

        double penalty = 0.0;
        for (int j = 0; j < n; j++)
        {
            if (j == i) continue;
            int harm = p[j].waiting - cfW[j];
            if (harm > 0) penalty += harm;
        }
        p[i].vcgPenalty = penalty / (double)p[i].trueService;
    }
}

// ─────────────────────────────────────────────
//  Strategic SJF
//  Schedules by reportedService, runs trueService.
//  Optional VCG penalties applied after scheduling.
// ─────────────────────────────────────────────

vector<Process> runStrategicSJF(vector<Process> p, bool vcg = false)
{
    int n = (int)p.size(), time = 0, done = 0;
    vector<bool> fin(n, false);
    while (done < n)
    {
        int idx = -1, best = numeric_limits<int>::max();
        for (int i = 0; i < n; i++)
            if (!fin[i] && p[i].arrival <= time && p[i].reportedService < best)
                { best = p[i].reportedService; idx = i; }
        if (idx == -1) { time++; continue; }
        p[idx].waiting    = time - p[idx].arrival;
        time             += p[idx].trueService;
        p[idx].finish     = time;
        p[idx].turnaround = time - p[idx].arrival;
        fin[idx] = true; done++;
    }
    if (vcg) applyVCGPenalties(p);
    return p;
}

vector<double> utilityFor(const vector<Process>& base,
                          const vector<double>&  mults,
                          bool vcg = false)
{
    vector<Process> p = base;
    for (int i = 0; i < (int)p.size(); i++)
    { p[i].multiplier = mults[i]; p[i].applyStrategy(); }
    vector<Process> r = runStrategicSJF(p, vcg);
    vector<double> u;
    for (const auto& x : r) u.push_back(x.utility());
    return u;
}

// ─────────────────────────────────────────────
//  Nash Equilibrium Solver
//  Best-response iteration with asymmetric
//  incentive-weighted initialisation.
//  Utility metric: normalised slowdown.
// ─────────────────────────────────────────────

struct NashResult
{
    vector<double> mults;
    vector<int>    waits;
    vector<double> utils;
    double         avgSlowdown;
    double         avgWaiting;
    int            iterations;
    bool           converged;
    NashResult() : avgSlowdown(0), avgWaiting(0), iterations(0), converged(false) {}
};

NashResult solveNash(const vector<Process>& base, bool vcg = false, int maxIter = 300)
{
    int n = (int)base.size();
    vector<double> grid;
    for (double m = MIN_MULT; m <= MAX_MULT + 1e-9; m += MULT_STEP)
        grid.push_back(round(m * 10.0) / 10.0);

    int maxB = 0;
    for (const auto& p : base) maxB = max(maxB, p.trueService);

    vector<double> mults(n);
    for (int i = 0; i < n; i++)
    {
        double w = incentiveWeight(base[i].trueService, maxB);
        mults[i] = max(MIN_MULT, min(MAX_MULT,
            round((1.0 - w * (1.0 - MIN_MULT)) * 10.0) / 10.0));
    }

    bool changed = true;
    int  iter    = 0;
    while (changed && iter < maxIter)
    {
        changed = false; iter++;
        for (int i = 0; i < n; i++)
        {
            double bestMult = mults[i], bestUtil = -1e18;
            for (double g : grid)
            {
                vector<double> trial = mults;
                trial[i] = g;
                double u = utilityFor(base, trial, vcg)[i];
                if (u > bestUtil) { bestUtil = u; bestMult = g; }
            }
            if (abs(bestMult - mults[i]) > 1e-9)
                { mults[i] = bestMult; changed = true; }
        }
    }

    vector<Process> fp = base;
    for (int i = 0; i < n; i++)
    { fp[i].multiplier = mults[i]; fp[i].applyStrategy(); }
    fp = runStrategicSJF(fp, vcg);

    vector<int> fw; vector<double> fu;
    double avgW = 0.0, avgS = 0.0;
    for (const auto& x : fp)
    {
        fw.push_back(x.waiting);
        fu.push_back(x.utility());
        avgW += x.waiting;
        avgS += slowdown(x.waiting, x.trueService);
    }
    NashResult nr;
    nr.mults       = mults;
    nr.waits       = fw;
    nr.utils       = fu;
    nr.avgSlowdown = avgS / n;
    nr.avgWaiting  = avgW / n;
    nr.iterations  = iter;
    nr.converged   = !changed;
    return nr;
}

// ─────────────────────────────────────────────
//  Repeated Game
//  Four standard strategies:
//    COOPERATE    — always truthful
//    DEFECT       — always max-underreport
//    GRIM_TRIGGER — cooperate until defection detected, then defect forever
//    TIT_FOR_TAT  — mirror most-underreporting opponent each round
//  Payoffs discounted by factor delta per round.
//  Computes critical delta* below which cooperation breaks down.
// ─────────────────────────────────────────────

enum RepeatedStrategy { COOPERATE, DEFECT, GRIM_TRIGGER, TIT_FOR_TAT };

string stratName(RepeatedStrategy s)
{
    switch (s)
    {
        case COOPERATE:    return "COOPERATE";
        case DEFECT:       return "DEFECT";
        case GRIM_TRIGGER: return "GRIM_TRIG";
        case TIT_FOR_TAT:  return "TIT4TAT";
    }
    return "?";
}

struct RepeatedRound
{
    int            round;
    vector<double> mults;
    vector<int>    waits;
    double         avgWait;
    double         avgSlow;
    vector<double> discCumUtil;
    RepeatedRound() : round(0), avgWait(0), avgSlow(0) {}
};

struct RepeatedResult
{
    vector<RepeatedRound>    history;
    vector<RepeatedStrategy> strategies;
    vector<double>           finalMults;
    double                   criticalDelta;
    bool                     cooperationSurvived;
    RepeatedResult() : criticalDelta(1.0), cooperationSurvived(false) {}
};

RepeatedResult repeatedGame(const vector<Process>&   base,
                            vector<RepeatedStrategy> strategies,
                            int    rounds = 50,
                            double delta  = 0.9,
                            bool   vcg    = false)
{
    int n = (int)base.size();
    vector<double> mults(n, 1.0);
    for (int i = 0; i < n; i++)
        if (strategies[i] == DEFECT) mults[i] = MIN_MULT;

    bool grimTriggered = false;
    vector<double> discCumUtil(n, 0.0);

    RepeatedResult result;
    result.strategies = strategies;

    for (int r = 1; r <= rounds; r++)
    {
        if (r > 1)
            for (double m : result.history.back().mults)
                if (m < 1.0 - 1e-9) { grimTriggered = true; break; }

        for (int i = 0; i < n; i++)
        {
            switch (strategies[i])
            {
                case COOPERATE:
                    mults[i] = 1.0; break;
                case DEFECT:
                    mults[i] = MIN_MULT; break;
                case GRIM_TRIGGER:
                    mults[i] = grimTriggered ? MIN_MULT : 1.0; break;
                case TIT_FOR_TAT:
                    if (r == 1) { mults[i] = 1.0; break; }
                    {
                        double minOpp = 1.0;
                        for (int j = 0; j < n; j++)
                            if (j != i)
                                minOpp = min(minOpp, result.history.back().mults[j]);
                        mults[i] = minOpp;
                    }
                    break;
            }
        }

        vector<Process> p = base;
        for (int i = 0; i < n; i++)
        { p[i].multiplier = mults[i]; p[i].applyStrategy(); }
        p = runStrategicSJF(p, vcg);

        double avgW = 0.0, avgS = 0.0;
        vector<int> waits;
        for (int i = 0; i < n; i++)
        {
            waits.push_back(p[i].waiting);
            avgW += p[i].waiting;
            avgS += slowdown(p[i].waiting, p[i].trueService);
            discCumUtil[i] += pow(delta, r - 1) * p[i].utility();
        }
        RepeatedRound rec;
        rec.round       = r;
        rec.mults       = mults;
        rec.waits       = waits;
        rec.avgWait     = avgW / n;
        rec.avgSlow     = avgS / n;
        rec.discCumUtil = discCumUtil;
        result.history.push_back(rec);
    }

    result.finalMults = mults;

    double critDelta = 1.0;
    for (double d = 0.99; d >= 0.01; d -= 0.01)
    {
        bool allOK = true;
        for (int i = 0; i < n; i++)
        {
            vector<double> devMults(n, 1.0);
            devMults[i] = MIN_MULT;
            double defUtil = utilityFor(base, devMults, false)[i];

            vector<double> coopMults(n, 1.0);
            double coopUtil = utilityFor(base, coopMults, false)[i];
            double stream = 0.0;
            for (int t = 0; t < 50; t++) stream += pow(d, t) * coopUtil;

            if (stream < defUtil) { allOK = false; break; }
        }
        if (allOK) { critDelta = d; break; }
    }
    result.criticalDelta = critDelta;

    bool coop = true;
    for (int i = 0; i < n; i++)
        if ((strategies[i] == GRIM_TRIGGER || strategies[i] == TIT_FOR_TAT)
            && abs(result.finalMults[i] - 1.0) > 1e-9)
            { coop = false; break; }
    result.cooperationSurvived = coop;

    return result;
}

// ─────────────────────────────────────────────
//  Price of Anarchy
// ─────────────────────────────────────────────

double PoA(double opt, double eq)
{
    if (opt <= 0.0) return numeric_limits<double>::infinity();
    return eq / opt;
}

// ─────────────────────────────────────────────
//  Output Helpers
// ─────────────────────────────────────────────

void printMetrics(const string& label, const Metrics& m)
{
    cout << "  " << left << setw(30) << label
         << "  avgWait="     << fixed << setprecision(3) << setw(7) << m.avgWaiting
         << "  avgSlowdown=" << fixed << setprecision(3) << setw(7) << m.avgSlowdown
         << "  fairness="    << fixed << setprecision(3) << m.fairness << "\n";
}

void printNash(const NashResult& ne, double srtSlow,
               const vector<Process>& base, const string& label)
{
    int n    = (int)base.size();
    int maxB = 0;
    for (const auto& p : base) maxB = max(maxB, p.trueService);

    cout << "\n  ── " << label << " ──\n";
    cout << "  Converged: " << (ne.converged ? "YES" : "NO")
         << "  (" << ne.iterations << " iterations)\n";

    cout << "  Mults: [";
    for (int i = 0; i < n; i++)
    {
        double w = incentiveWeight(base[i].trueService, maxB);
        cout << fixed << setprecision(1) << ne.mults[i]
             << "(iw=" << setprecision(2) << w << ")"
             << (i+1<n ? ", " : "");
    }
    cout << "]\n";

    cout << "  Waits: [";
    for (int i = 0; i < n; i++)
        cout << ne.waits[i] << (i+1<n ? ", " : "");
    cout << "]\n";

    cout << "  Utils: [";
    for (int i = 0; i < n; i++)
        cout << fixed << setprecision(3) << ne.utils[i] << (i+1<n ? ", " : "");
    cout << "]\n";

    cout << "  Avg slowdown: " << fixed << setprecision(3) << ne.avgSlowdown
         << "   PoA: " << fixed << setprecision(4) << PoA(srtSlow, ne.avgSlowdown) << "\n";

    cout << "\n  " << setw(5)  << "PID"
         << setw(8)  << "mult"
         << setw(8)  << "iw"
         << setw(8)  << "wait"
         << setw(10) << "utility"
         << setw(16) << "stable?\n";
    cout << "  " << string(55, '-') << "\n";

    for (int i = 0; i < n; i++)
    {
        double curU    = ne.utils[i];
        bool   canDev  = false;
        double devMult = ne.mults[i];
        for (double g = MIN_MULT; g <= MAX_MULT + 1e-9; g += MULT_STEP)
        {
            double mg = round(g * 10.0) / 10.0;
            vector<double> trial = ne.mults; trial[i] = mg;
            if (utilityFor(base, trial, false)[i] > curU + 1e-9)
                { canDev = true; devMult = mg; break; }
        }
        double w = incentiveWeight(base[i].trueService, maxB);
        cout << "  " << setw(5)  << base[i].id
             << setw(8)  << fixed << setprecision(1) << ne.mults[i]
             << setw(8)  << fixed << setprecision(2) << w
             << setw(8)  << ne.waits[i]
             << setw(10) << fixed << setprecision(3) << curU
             << setw(16) << (canDev
                 ? "UNSTABLE→" + to_string(devMult).substr(0, 4)
                 : "STABLE")
             << "\n";
    }
}

void printRepeated(const string& label, const RepeatedResult& rg,
                   const vector<Process>& base)
{
    int n = (int)base.size();
    cout << "\n  ── " << label << " ──\n";
    cout << "  Strategies: ";
    for (int i = 0; i < n; i++)
        cout << "P" << base[i].id << "=" << stratName(rg.strategies[i])
             << (i+1<n ? "  " : "");
    cout << "\n";

    cout << "  " << setw(6)  << "Round"
         << setw(10) << "avgWait"
         << setw(12) << "avgSlow"
         << setw(36) << "multipliers\n";
    cout << "  " << string(64, '-') << "\n";

    int H = (int)rg.history.size();
    for (int r = 0; r < H; r++)
    {
        if (r != 0 && r != H-1 && r % 5 != 4) continue;
        const auto& rec = rg.history[r];
        cout << "  " << setw(6)  << rec.round
             << setw(10) << fixed << setprecision(2) << rec.avgWait
             << setw(12) << fixed << setprecision(3) << rec.avgSlow
             << "  [";
        for (int i = 0; i < (int)rec.mults.size(); i++)
            cout << fixed << setprecision(1) << rec.mults[i]
                 << (i+1<(int)rec.mults.size() ? "," : "");
        cout << "]\n";
    }

    cout << "  Discounted cumulative utility (d=0.9): [";
    const auto& last = rg.history.back();
    for (int i = 0; i < (int)last.discCumUtil.size(); i++)
        cout << fixed << setprecision(2) << last.discCumUtil[i]
             << (i+1<(int)last.discCumUtil.size() ? ", " : "");
    cout << "]\n";

    cout << "  Cooperation survived: "
         << (rg.cooperationSurvived ? "YES" : "NO") << "\n";
    cout << "  Critical d*: " << fixed << setprecision(3) << rg.criticalDelta
         << "  (GRIM_TRIGGER holds for d >= d*)\n";
}

// ─────────────────────────────────────────────
//  Main
// ─────────────────────────────────────────────

int main()
{
    const int QUANTUM = 2;

    int N;
    cout << "Enter number of processes: ";
    cin >> N;
    if (N <= 0) { cerr << "Error: need at least 1 process.\n"; return 1; }

    cout << "Enter processes as:  ID  ArrivalTime  BurstTime\n";
    vector<Process> base;
    for (int i = 0; i < N; i++)
    {
        int id, arr, burst;
        cin >> id >> arr >> burst;
        if (burst <= 0)
        { cerr << "Error: burst must be > 0 (P" << id << ")\n"; return 1; }
        base.emplace_back(id, arr, burst);
    }
    cout << "\n";

    int n    = (int)base.size();
    int maxB = 0;
    for (const auto& p : base) maxB = max(maxB, p.trueService);

    cout << "=== Game-Theoretic CPU Scheduling Simulator ===\n\n";

    // 1. Baseline Schedulers
    cout << "--- 1. BASELINE SCHEDULERS ---\n";
    Metrics fcfs = runFCFS(base);
    Metrics rr   = runRR(base, QUANTUM);
    Metrics srt  = runSRT(base);
    printMetrics("FCFS",                  fcfs);
    printMetrics("Round Robin (q=2)",     rr);
    printMetrics("SRT  [social optimum]", srt);

    // 2. Asymmetric Incentive Weights
    cout << "\n--- 2. ASYMMETRIC INCENTIVE WEIGHTS ---\n";
    cout << "  " << setw(5)  << "PID"
         << setw(8)  << "burst"
         << setw(10) << "iw"
         << setw(12) << "init_mult"
         << setw(28) << "interpretation\n";
    cout << "  " << string(63, '-') << "\n";
    for (const auto& p : base)
    {
        double w    = incentiveWeight(p.trueService, maxB);
        double init = max(MIN_MULT, min(MAX_MULT,
            round((1.0 - w * (1.0 - MIN_MULT)) * 10.0) / 10.0));
        string interp = (w > 0.7) ? "strong underreport incentive"
                      : (w > 0.3) ? "moderate incentive"
                                  : "weak  (near-truthful start)";
        cout << "  " << setw(5)  << p.id
             << setw(8)  << p.trueService
             << setw(10) << fixed << setprecision(2) << w
             << setw(12) << fixed << setprecision(1) << init
             << "  " << interp << "\n";
    }

    cout << "\n  Slowdown vs uniform multiplier:\n";
    cout << "  " << setw(12) << "mult"
         << setw(14) << "avgSlowdown"
         << setw(10) << "fairness\n";
    cout << "  " << string(36, '-') << "\n";
    for (double m = 1.0; m >= MIN_MULT - 1e-9; m -= 0.1)
    {
        double mg = round(m * 10.0) / 10.0;
        vector<Process> p = base;
        for (int i = 0; i < (int)p.size(); i++)
        { p[i].multiplier = mg; p[i].applyStrategy(); }
        vector<Process> r = runStrategicSJF(p, false);
        Metrics met = computeMetrics(r, r.back().finish);
        cout << "  " << setw(12) << fixed << setprecision(1) << mg
             << setw(14) << fixed << setprecision(3) << met.avgSlowdown
             << setw(10) << fixed << setprecision(3) << met.fairness << "\n";
    }

    // 3. Nash Equilibrium
    cout << "\n--- 3. NASH EQUILIBRIUM ---\n";

    NashResult neBase = solveNash(base, false);
    printNash(neBase, srt.avgSlowdown, base, "Without VCG Penalty");

    NashResult neVCG = solveNash(base, true);
    printNash(neVCG,  srt.avgSlowdown, base, "With VCG Penalty");

    cout << "\n  Comparison:\n";
    cout << "  " << left << setw(28) << "Scenario"
         << setw(14) << "avgSlowdown"
         << setw(10) << "PoA\n";
    cout << "  " << string(52, '-') << "\n";

    auto row = [&](const string& l, double s) {
        cout << "  " << setw(28) << l
             << setw(14) << fixed << setprecision(3) << s
             << setw(10) << fixed << setprecision(4) << PoA(srt.avgSlowdown, s) << "\n";
    };
    row("SRT (optimum)",    srt.avgSlowdown);
    row("NE without VCG",   neBase.avgSlowdown);
    row("NE with VCG",      neVCG.avgSlowdown);

    // 4. Repeated Game
    cout << "\n--- 4. REPEATED GAME (d=0.9) ---\n";

    { auto r = repeatedGame(base, vector<RepeatedStrategy>(n, COOPERATE), 50, 0.9, false);
      printRepeated("All COOPERATE", r, base); }

    { auto r = repeatedGame(base, vector<RepeatedStrategy>(n, DEFECT), 50, 0.9, false);
      printRepeated("All DEFECT", r, base); }

    { auto r = repeatedGame(base, vector<RepeatedStrategy>(n, GRIM_TRIGGER), 50, 0.9, false);
      printRepeated("All GRIM_TRIGGER", r, base); }

    { vector<RepeatedStrategy> s(n, GRIM_TRIGGER); s[0] = DEFECT;
      auto r = repeatedGame(base, s, 50, 0.9, false);
      printRepeated("P1=DEFECT  rest=GRIM_TRIGGER", r, base); }

    { vector<RepeatedStrategy> s(n, TIT_FOR_TAT);
      if (n >= 2) s[0] = DEFECT;
      auto r = repeatedGame(base, s, 50, 0.9, false);
      printRepeated("P1=DEFECT  rest=TIT_FOR_TAT", r, base); }

    { auto r = repeatedGame(base, vector<RepeatedStrategy>(n, GRIM_TRIGGER), 50, 0.9, true);
      printRepeated("All GRIM_TRIGGER + VCG", r, base); }

    // 5. Summary
    cout << "\n--- 5. SUMMARY ---\n";
    cout << "  " << left << setw(36) << "Scenario"
         << setw(14) << "avgSlowdown"
         << setw(8)  << "PoA\n";
    cout << "  " << string(58, '-') << "\n";
    row("SRT (social optimum)",     srt.avgSlowdown);
    row("FCFS (truthful baseline)", fcfs.avgSlowdown);
    row("NE without VCG",           neBase.avgSlowdown);
    row("NE with VCG",              neVCG.avgSlowdown);

    { vector<RepeatedStrategy> s(n, GRIM_TRIGGER);
      auto r = repeatedGame(base, s, 50, 0.9, false);
      cout << "\n  Critical discount factor d*: "
           << fixed << setprecision(3) << r.criticalDelta << "\n";
      cout << "  Cooperation sustained at d=0.9: "
           << (r.cooperationSurvived ? "YES" : "NO") << "\n"; }

    return 0;
}