#include <boost/foreach.hpp>
#include <boost/make_shared.hpp>
#include <openrave/planningutils.h>
#include "ParabolicSmoother.h"

using boost::make_shared;
using OpenRAVE::EnvironmentBasePtr;
using OpenRAVE::RobotBasePtr;
using OpenRAVE::TrajectoryBasePtr;

namespace {

template <typename Tout, typename Tin>
static std::vector<Tout> Convert(std::vector<Tin> const &vin)
{
    std::vector<Tout> vout(vin.size());

    for (size_t i = 0; i < vin.size(); ++i) {
        vout[i] = vin[i];
    }

    return vout;
}

void ConvertWaypoint(TrajectoryBasePtr const &traj,
                     ParabolicRamp::DynamicPath const &dynamic_path,
                     double t, double dt)
{
    size_t const num_dof = dynamic_path.velMax.size();

    OpenRAVE::ConfigurationSpecification const cspec
        = traj->GetConfigurationSpecification();

    ParabolicRamp::Vector q;
    dynamic_path.Evaluate(t, q);
    BOOST_ASSERT(q.size() == num_dof);

    ParabolicRamp::Vector qd;
    dynamic_path.Derivative(t, qd);
    BOOST_ASSERT(qd.size() == num_dof);

    std::vector<OpenRAVE::dReal> waypoint(cspec.GetDOF());
    for (size_t idof = 0; idof < num_dof; ++idof) {
        waypoint[idof] = q[idof];
        waypoint[idof + num_dof] = qd[idof];
    }

    waypoint[2 * num_dof] = dt;

    traj->Insert(traj->GetNumWaypoints(), waypoint, false);
}

OpenRAVE::dReal FixLimit(OpenRAVE::dReal const &x,
                         OpenRAVE::dReal const &x_min,
                         OpenRAVE::dReal const &x_max,
                         OpenRAVE::dReal const &tolerance)
{
    if (x < x_min) {
        if (x < x_min - tolerance) {
            // TODO: error
            return x_min;
        } else {
            return x_min;
        }
    } else if (x > x_max) {
        if (x > x_max + tolerance) {
            // TODO: error;
            return x_max;
        } else {
            return x_max;
        }
    } else {
        return x;
    }
}

bool NeedsBlend(ParabolicRamp::ParabolicRampND const &ramp_nd)
{
    static double const velocity_epsilon = 1e-12;

    for (size_t idof = 0; idof < ramp_nd.dx1.size(); ++idof) {
        if (std::fabs(ramp_nd.dx1[idof]) > velocity_epsilon) {
            return false;
        }
    }
    return true;
}

bool TryBlend(ParabolicRamp::DynamicPath &dynamic_path,
              ParabolicRamp::RampFeasibilityChecker &ramp_checker,
              int attempt, double dt_shortcut)
{
    size_t const num_ramps = dynamic_path.ramps.size();
    double const t_max = dynamic_path.GetTotalTime();
    double t = 0;

    for (size_t iwaypoint = 0; iwaypoint < num_ramps - 1; ++iwaypoint) {
        ParabolicRamp::ParabolicRampND &ramp_nd
                = dynamic_path.ramps[iwaypoint];
        t += ramp_nd.endTime;

        if (NeedsBlend(ramp_nd) && ramp_nd.blendAttempts == attempt) {
            double const t1 = std::max(t - dt_shortcut, 0.);
            double const t2 = std::min(t + dt_shortcut, t_max);

            bool const success = dynamic_path.TryShortcut(t1, t2, ramp_checker);

            RAVELOG_DEBUG("Blending [ %.3f, %.3f ] transition between ramp %d"
                          " and %d at t = %.3f with dt = %.3f: %s.\n",
                t1, t2, iwaypoint, iwaypoint + 1, t, dt_shortcut,
                (success) ? "succeeded" : "failed"
            );

            if (success) {
                return true;
            } else {
                ramp_nd.blendAttempts++;
            }
        }
    }

    return false;
}

void BlendTransitions(ParabolicRamp::DynamicPath &dynamic_path,
                      ParabolicRamp::RampFeasibilityChecker &ramp_checker,
                      double dt_shortcut_max, int num_attempts)
{
    // Mark all of the ramps in the initial trajectory as "original". We'll
    // only try to blend transitions between these ramps.
    BOOST_FOREACH (ParabolicRamp::ParabolicRampND &ramp_nd,
                   dynamic_path.ramps) {
        ramp_nd.blendAttempts = 0;
    }

    double dt_shortcut = dt_shortcut_max;

    for (int attempt = 0; attempt < num_attempts; ++attempt) {
        RAVELOG_DEBUG("Blending pass %d (dt = %.3f) on trajectory with %d ramps"
                      " and duration %.3f s.\n",
            attempt, dt_shortcut, dynamic_path.ramps.size(),
            dynamic_path.GetTotalTime()
        );

        while (TryBlend(dynamic_path, ramp_checker, attempt, dt_shortcut));

        dt_shortcut /= 2.;
    }
}

}

namespace or_parabolicsmoother
{

/*
 * ORFeasibilityChecker 
 */
ORFeasibilityChecker::ORFeasibilityChecker(
        OpenRAVE::EnvironmentBasePtr const &env,
        OpenRAVE::PlannerBase::PlannerParametersConstPtr const &params)
    : env_(env)
    , params_(params)
{
}

bool ORFeasibilityChecker::ConfigFeasible(ParabolicRamp::Vector const &x)
{
    std::vector<OpenRAVE::dReal> const or_x = Convert<OpenRAVE::dReal>(x);
    std::vector<OpenRAVE::dReal> const empty;

    return !params_->CheckPathAllConstraints(
        x, x, empty, empty, 0, OpenRAVE::IT_OpenStart
    );
}

bool ORFeasibilityChecker::SegmentFeasible(ParabolicRamp::Vector const &a,
                                           ParabolicRamp::Vector const &b)
{
    std::vector<OpenRAVE::dReal> const or_a = Convert<OpenRAVE::dReal>(a);
    std::vector<OpenRAVE::dReal> const or_b = Convert<OpenRAVE::dReal>(b);
    std::vector<OpenRAVE::dReal> const empty;

    return !params_->CheckPathAllConstraints(
        a, b, empty, empty, 0, OpenRAVE::IT_Closed
    );
}

/*
 * ParabolicSmoother
 */
ParabolicSmoother::ParabolicSmoother(EnvironmentBasePtr penv)
    : OpenRAVE::PlannerBase(penv)
{
}

bool ParabolicSmoother::InitPlan(RobotBasePtr robot,
                                 PlannerParametersConstPtr params)
{
    parameters_ = params;
    return true;
}

bool ParabolicSmoother::InitPlan(RobotBasePtr robot,
                                 std::istream &input)
{
    PlannerParametersPtr const params = make_shared<PlannerParameters>();

    // Deserialize the PlannerParameters once. We only do this to set
    // _configurationspecification for the next step, so we put the stream back
    // where it was.
    int const marker = input.tellg();
    input >> *params;
    input.seekg(marker);

    // The CheckPathAllConstraints function returns "true" (cast to an integer)
    // if the _checkpathvelocityconstraintsfn is NULL. This value defaults to
    // NULL and is not serialized in PlannerParameters. We re-initialize the
    // parameters with the default values.
    params->SetConfigurationSpecification(
        GetEnv(), params->_configurationspecification);

    // Restore any parameters that may have be overwritten by
    // SetConfigurationSpecification.
    input >> *params;

    return InitPlan(robot, params);
}

OpenRAVE::PlannerStatus ParabolicSmoother::PlanPath(TrajectoryBasePtr traj)
{
    using OpenRAVE::ConfigurationSpecification;
    using OpenRAVE::KinBodyPtr;

    EnvironmentBasePtr const env = GetEnv();
    ConfigurationSpecification pos_cspec
        = parameters_->_configurationspecification;

    // TODO: How do we do this properly?
    BOOST_FOREACH (ConfigurationSpecification::Group &group,
                   pos_cspec._vgroups) {
        group.interpolation = "quadratic";
    }

    ConfigurationSpecification vel_cspec
        = pos_cspec.ConvertToVelocitySpecification();

    size_t num_dof = pos_cspec.GetDOF();
    RAVELOG_DEBUG("Detected %d DOFs.\n", num_dof);

    ParabolicRamp::DynamicPath dynamic_path;
    RAVELOG_DEBUG("Setting velocity and acceleration limits.\n");
    dynamic_path.Init(
        Convert<double>(parameters_->_vConfigVelocityLimit),
        Convert<double>(parameters_->_vConfigAccelerationLimit)
    );
    BOOST_ASSERT(dynamic_path.velMax.size() == num_dof);
    BOOST_ASSERT(dynamic_path.accMax.size() == num_dof);

    RAVELOG_DEBUG("Setting joint limits.\n");
    dynamic_path.SetJointLimits(
        Convert<double>(parameters_->_vConfigLowerLimit),
        Convert<double>(parameters_->_vConfigUpperLimit)
    );
    BOOST_ASSERT(dynamic_path.xMin.size() == num_dof);
    BOOST_ASSERT(dynamic_path.xMax.size() == num_dof);
    
    // Copy milestones into the DynamicPath. This assumes that the input
    // trajectory is piecewise linear and stops at each waypoint.
    // TODO: What about velocities?
    std::vector<ParabolicRamp::Vector> milestones(traj->GetNumWaypoints());

    for (size_t iwaypoint = 0; iwaypoint < traj->GetNumWaypoints(); ++iwaypoint) {
        std::vector<OpenRAVE::dReal> waypoint;
        traj->GetWaypoint(iwaypoint, waypoint, pos_cspec);

        // Fix small joint limit violations.
        for (size_t idof = 0; idof < num_dof; ++idof) {
            waypoint[idof] = FixLimit(
                waypoint[idof],
                parameters_->_vConfigLowerLimit[idof],
                parameters_->_vConfigUpperLimit[idof],
                parameters_->_vConfigResolution[idof]
            );
        }

        BOOST_ASSERT(waypoint.size() == num_dof);
        milestones[iwaypoint] = Convert<double>(waypoint);
    }

    RAVELOG_DEBUG("Setting %d milestones.\n", milestones.size());
    dynamic_path.SetMilestones(milestones);

    if (!dynamic_path.IsValid()) {
        throw OpenRAVE::openrave_exception(
            "Converted DynamicPath is not valid.",
            OpenRAVE::ORE_Failed
        );
    }

    // Choose the most conservative resolution because RampFeasibilityChecker
    // does not support per-DOF resolutions.
    OpenRAVE::dReal const tolerance = *std::min_element(
        parameters_->_vConfigResolution.begin(),
        parameters_->_vConfigResolution.end()
    );
    BOOST_ASSERT(tolerance > 0.);
    RAVELOG_DEBUG("Creating collision checker with resolution %f.\n",
                  tolerance);

    // Create feasibiility checkers that evaluate the OpenRAVE environment.
    ORFeasibilityChecker base_checker(env, parameters_);
    ParabolicRamp::RampFeasibilityChecker ramp_checker(&base_checker, tolerance);

    // TODO: Split this into multiple iterations so we can call callbacks.
    // Shortcut using maximum number of iterations. According to OpenRAVE spec:
    // If 0 or less, planner chooses best iterations.
    int max_iterations = (parameters_->_nMaxIterations > 0) ?
                          parameters_->_nMaxIterations : DEFAULT_MAX_ITERATIONS;

    RAVELOG_DEBUG("Shortcutting for %d iterations.\n", max_iterations);

    // Perform actual shortcut operation in this loop.
    dynamic_path.Shortcut(max_iterations, ramp_checker);

    // Blend any transitions that we missed while shortcutting.
    // TODO: Don't hard-code these parameters.
    BlendTransitions(dynamic_path, ramp_checker, 0.5, 4);

    if (!dynamic_path.IsValid()) {
        return OpenRAVE::PS_Failed;
    }

    // Clear the trajectory to write in the output.
    traj->Remove(0, traj->GetNumWaypoints());
    BOOST_ASSERT(traj->GetNumWaypoints() == 0);

    // Convert back to an OpenRAVE trajectory.
    OpenRAVE::ConfigurationSpecification output_cspec = pos_cspec + vel_cspec;
    output_cspec.AddDeltaTimeGroup();

    RAVELOG_DEBUG("Creating output trajectory (duration: %f).\n",
                  dynamic_path.GetTotalTime());
    OpenRAVE::planningutils::ConvertTrajectorySpecification(traj, output_cspec);

    // Generate all ramp start/end times and acceleration switching times.
    std::set<double> sample_times;

    double t = 0.;
    sample_times.insert(t);

    BOOST_FOREACH (ParabolicRamp::ParabolicRampND const &ramp_nd,
                   dynamic_path.ramps) {
        BOOST_FOREACH (ParabolicRamp::ParabolicRamp1D const &ramp_1d,
                       ramp_nd.ramps) {
            sample_times.insert(t + ramp_1d.tswitch1);
            sample_times.insert(t + ramp_1d.tswitch2);
        }

        t += ramp_nd.endTime;
        sample_times.insert(t);
    }

    RAVELOG_DEBUG("Detected %d critical ramp transition points.\n",
                  sample_times.size());

    // Insert the critical points into the output trajectory.
    double prev_t = 0.;
    ConvertWaypoint(traj, dynamic_path, prev_t, 0.);

    BOOST_FOREACH (double const t, sample_times) {
        double const dt = t - prev_t;
        BOOST_ASSERT(dt >= 0.);
        prev_t = t;

        ConvertWaypoint(traj, dynamic_path, t, dt);
    }

    return OpenRAVE::PS_HasSolution;
}

OpenRAVE::PlannerBase::PlannerParametersConstPtr
    ParabolicSmoother::GetParameters () const
{
    return parameters_;
}

}
