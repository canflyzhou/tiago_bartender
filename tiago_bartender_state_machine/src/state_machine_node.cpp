#include <ros/ros.h>
#include <functional>
#include <std_msgs/String.h>
#include <tiago_bartender_navigation/MoveToTargetAction.h>
#include <tiago_bartender_navigation/FindClosestTargetAction.h>
//#include <tiago_bartender_mtc/PickBottleAction.h>
#include <tiago_bartender_mtc/PickAction.h>
#include <tiago_bartender_mtc/PlaceAction.h>
#include <tiago_bartender_mtc/PourAction.h>
#include <actionlib/client/simple_action_client.h>
#include <std_srvs/SetBool.h>
#include <std_srvs/Empty.h>
#include <geometry_msgs/PointStamped.h>
#include <visualization_msgs/Marker.h>
#include <trajectory_msgs/JointTrajectory.h>
#include <control_msgs/FollowJointTrajectoryAction.h>
#include <tiago_bartender_speech/BartenderSpeechAction.h>
#include <tiago_bartender_menu/TakeOrderAction.h>
#include <tiago_bartender_behavior/LookAt.h>
#include <queue>
#include <move_base_msgs/MoveBaseAction.h>
#include <random>
#include <chrono>

class StateMachine
{
public:
  StateMachine() : mtt_client_("move_to_target", true),
                   fct_client_("find_closest_target", true),
                   fjt_client_("torso_controller/follow_joint_trajectory", true),
                   bs_client_("bartender_speech_action", true),
                   to_client_("menu/take_order", true),
                   mb_client_("move_base", true),
                   unif_(0, 1),
                   person_detected_(false)
  {
    // getting and parsing parameters
    ros::NodeHandle pn("~");
    XmlRpc::XmlRpcValue recipes;
    pn.getParam("recipes", recipes);
    ROS_ASSERT(recipes.getType() == XmlRpc::XmlRpcValue::TypeStruct);

    for (XmlRpc::XmlRpcValue::iterator i=recipes.begin(); i!=recipes.end(); ++i)
    {
      ROS_ASSERT(i->second.getType()==XmlRpc::XmlRpcValue::TypeArray);
      recipes_[static_cast<std::string>(i->first)];
      for(int j=0; j<i->second.size(); ++j)
      {
        XmlRpc::XmlRpcValue raw_ingredient = i->second[j];
        ROS_ASSERT(raw_ingredient.getType()==XmlRpc::XmlRpcValue::TypeStruct);
        std::pair<std::string, double> ingredient(static_cast<std::string>(raw_ingredient.begin()->first), static_cast<double>(raw_ingredient.begin()->second));

        recipes_[static_cast<std::string>(i->first)].push(ingredient);
      }
    }

    double idle_zone_min_x;
    double idle_zone_max_x;
    double idle_zone_min_y;
    double idle_zone_max_y;
    pn.getParam("idle_zone_min_x", idle_zone_min_x);
    pn.getParam("idle_zone_max_x", idle_zone_max_x);
    pn.getParam("idle_zone_min_y", idle_zone_min_y);
    pn.getParam("idle_zone_max_y", idle_zone_max_y);

    unif_idle_x_ = std::uniform_real_distribution<double> (idle_zone_min_x, idle_zone_max_x);
    unif_idle_y_ = std::uniform_real_distribution<double> (idle_zone_min_y, idle_zone_max_y);

    marker_pub_ = nh_.advertise<visualization_msgs::Marker>("bartender_state_marker", 0);
    look_at_client_ = nh_.serviceClient<tiago_bartender_behavior::LookAt>("head_controller/look_at_service");
    person_detection_sub_ = nh_.subscribe("person_detection", 1000, &StateMachine::person_detection_cb, this);

    // prepare random number generator for random idle states
    uint64_t timeSeed = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    std::seed_seq ss{uint32_t(timeSeed & 0xffffffff), uint32_t(timeSeed>>32)};
    rng_.seed(ss);

    marker_.header.frame_id = "base_footprint";
    marker_.id = 0;
    marker_.type = visualization_msgs::Marker::TEXT_VIEW_FACING;
    marker_.action = visualization_msgs::Marker::ADD;
    marker_.pose.position.x = 0.25;
    marker_.pose.position.y = 0.25;
    marker_.pose.position.z = 2.0;
    marker_.pose.orientation.x = 0.0;
    marker_.pose.orientation.y = 0.0;
    marker_.pose.orientation.z = 0.0;
    marker_.pose.orientation.w = 1.0;
    marker_.scale.x = 0.25;
    marker_.scale.y = 0.25;
    marker_.scale.z = 0.25;
    marker_.color.a = 1.0;
    marker_.color.r = 1.0;
    marker_.color.g = 0.0;
    marker_.color.b = 0.0;

    // without this sleep the first marker is not published
    ros::Duration(1.0).sleep();

    while(!mtt_client_.waitForServer(ros::Duration(1.0)) || !fct_client_.waitForServer(ros::Duration(1.0)) || !fjt_client_.waitForServer(ros::Duration(1.0)) || !bs_client_.waitForServer(ros::Duration(1.0)) || !to_client_.waitForServer(ros::Duration(1.0)) || !mb_client_.waitForServer(ros::Duration(1.0)))
    {
      ROS_ERROR_STREAM("tiago bartender state machine waits for all action servers to start.");
    }
  }

  void run()
  {
    while(true)
    {
      state(this);
    }
  }


private:
  void state_init()
  {
    ROS_INFO_STREAM("idling");
    publish_marker("state_init");
    extend_torso();
    look_at_target("forward");
    state = &StateMachine::state_idle_manager;
  }

  void state_idle_manager()
  {
    ROS_INFO_STREAM("state_idle_manager");
    publish_marker("state_idle_manager");
    if(get_rand() <= 0.2)
    {
      state = &StateMachine::state_idle_joke;
      return;
    }
    if(!parked_bottle_poses_.empty() && get_rand() <= 0.3)
    {
      state = &StateMachine::state_idle_return_parked_bottle;
      return;
    }
    state = &StateMachine::state_idle_random_pose;
  }

  void state_idle_return_parked_bottle()
  {
    ROS_INFO_STREAM("state_idle_return_parked_bottle");
    publish_marker("state_idle_return_parked_bottle");
    if(person_detected_)
    {
      state = &StateMachine::state_move_to_person;
      return;
    }
    state = &StateMachine::state_idle_manager;
  }

  void state_idle_random_pose()
  {
    ROS_INFO_STREAM("state_idle_random_pose");
    publish_marker("state_idle_random_pose");
    move_base_msgs::MoveBaseGoal target;
    target.target_pose.header.frame_id = "map";
    target.target_pose.pose.position.x = unif_idle_x_(rng_);
    target.target_pose.pose.position.y = unif_idle_y_(rng_);
    target.target_pose.pose.orientation.z = 1.0;
    movebase(target);
    look_at_target("look_around");
    ros::Time begin = ros::Time::now();
    while(ros::Time::now() - begin < ros::Duration(60.0))
    {
      if(person_detected_)
      {
        state = &StateMachine::state_move_to_person;
        return;
      }
    }
    state = &StateMachine::state_idle_manager;
  }

  void state_idle_joke()
  {
    ROS_INFO_STREAM("state_idle_joke");
    publish_marker("state_idle_joke");
    voice_command("joke");

    if(person_detected_)
    {
      state = &StateMachine::state_move_to_person;
      return;
    }
    state = &StateMachine::state_idle_manager;
  }

  void state_move_to_person()
  {
    ROS_INFO_STREAM("staet_move_to_person");
    publish_marker("state_move_to_person");
    move_to_person();
    current_customer_position_ = last_person_position_;
    state = [](StateMachine* m) { m->state_ask_order(0); };
  }

  void state_ask_order(size_t iteration)
  {
    ROS_INFO_STREAM("state_ask_order");
    publish_marker("state_ask_order");
    voice_command("ask_order");
    state = [iteration](StateMachine* m) { m->state_take_order(iteration); };
  }

  void state_menu_not_found(size_t iteration)
  {
    ROS_INFO_STREAM("state_menu_not_found");
    publish_marker("state_menu_not_found");
    voice_command("menu_not_found");
    state = [iteration](StateMachine* m) { m->state_take_order(iteration); };
  }
  
  void state_abort_order()
  {
    ROS_INFO_STREAM("state_abort_order");
    publish_marker("state_abort_order");
    voice_command("abort_order");
    state = &StateMachine::state_idle_manager;
  }

  void state_take_order(size_t iteration)
  {
    ROS_INFO_STREAM("state_take_order");
    publish_marker("state_take_order");
    ++iteration;
    // Point head down to look at menu
    look_at_target("down");

    // take order action
    tiago_bartender_menu::TakeOrderGoal goal;
    goal.timeout = ros::Duration(30.0);
    to_client_.sendGoal(goal);
    while(!to_client_.waitForResult(ros::Duration(20.0)))
      ROS_INFO("Waiting for take order action result.");

    auto result = to_client_.getResult();
    ROS_INFO_STREAM("Result of order action, status: " << result->status << " selection: " << result->selection);

    // Point head to look at the customer
    look_at_target("", last_person_position_);

    // process result and choose next state accordingly
    if((result->status == "timeout" || result->status == "no_menu_card_detected") && iteration >= 3)
    {
      state = &StateMachine::state_abort_order;
      return;
    }
    else if(result->status == "timeout")
    {
      state = [iteration](StateMachine* m) { m->state_ask_order(iteration); };
      return;
    }
    else if(result->status == "no_menu_card_detected")
    {
      state = [iteration](StateMachine* m) { m->state_menu_not_found(iteration); };
      return;
    }

    // successful order
    auto it = recipes_.find(result->selection);
    // check if drink has bottles mapped to it
    if(it == recipes_.end())
    {
      ROS_ERROR_STREAM("Selected drink is not in state machine map");
      state = [iteration](StateMachine* m) { m->state_ask_order(iteration); };
    }
    current_ingredients_ = it->second;

    state = &StateMachine::state_move_to_bottle;
  }

  void state_move_to_bottle()
  {
    ROS_INFO_STREAM("state_move_to_bottle");
    publish_marker("state_move_to_bottle");
    look_at_target("forward");
    std::string bottle_name = current_ingredients_.front().first;
    voice_command(bottle_name);
    bool success = move_to_target(bottle_name, false, last_bottle_pose_);
    if(success)
    {
      // look at bottle
      geometry_msgs::PointStamped target;
      target.header = last_bottle_pose_.header;
      target.point = last_bottle_pose_.pose.position;
      look_at_target("", target);

      current_ingredients_.pop();
      state = [bottle_name](StateMachine* m) { m->state_update_scene(bottle_name); };
    }
    else
    {
      move_to_home();
      state = &StateMachine::state_move_to_bottle;
    }
  }

  void state_update_scene(const std::string& bottle_name)
  {
    ROS_INFO_STREAM("state_update_scene");
    publish_marker("state_update_scene");
    // todo: replace this place holder with actual update method
    ros::NodeHandle node;
    ros::ServiceClient client = node.serviceClient<std_srvs::Empty>("dummy_planning_scene/update_planning_scene");
    std_srvs::Empty srv;
    if (!client.call(srv))
    {
      ROS_ERROR("Failed to call service update_planning_scene");
      state = [bottle_name](StateMachine* m) { m->state_update_scene(bottle_name); };
      return;
    }

    // todo: check if the correct bottle was observed by the recognition
    if(false)
    {
      voice_command("bottle_not_found");
      state = [bottle_name](StateMachine* m) { m->state_update_scene(bottle_name); };
      return;
    }

    state = [bottle_name](StateMachine* m) { m->state_grasp_bottle(bottle_name); };
  }
 
  void state_grasp_bottle(const std::string& bottle_name)
  {
    ROS_INFO_STREAM("state_grasp_bottle");
    publish_marker("state_grasp_bottle");

    // temporary
    state = &StateMachine::state_move_to_glass;

    /*actionlib::SimpleActionClient<tiago_bartender_mtc::PickAction> client("tiago_bottle_pick", true);
    client.waitForServer();
    tiago_bartender_mtc::PickGoal goal;
    goal.object_id = bottle_name;
    client.sendGoal(goal);
    while(!client.waitForResult(ros::Duration(5.0)))
      ROS_INFO("Waiting for bottle grasp result.");

    int res = client.getResult()->result.result;
    if(res == tiago_bartender_mtc::Result::SUCCESS)
    {
      ROS_INFO("Successfully grasped to bottle.");
      state = &StateMachine::state_move_to_glass;
    }
    else if(res == tiago_bartender_mtc::Result::UNREACHABLE)
    {
      ROS_ERROR_STREAM("Could not reach object.");
      // todo: reposition robot in different pose
      state = [bottle_name](StateMachine* m) { m->state_grasp_bottle(bottle_name); };
    }
    else if(res == tiago_bartender_mtc::Result::NO_PLAN_FOUND || res == tiago_bartender_mtc::Result::EXECUTION_FAILED)
    {
      ROS_ERROR("Grasping bottle aborted.");
      state = [bottle_name](StateMachine* m) { m->state_grasp_bottle(bottle_name); };
    }*/
  }
  
  void state_move_to_glass()
  {
    ROS_INFO_STREAM("state_move_to_glass");
    publish_marker("state_move_to_glass");
    look_at_target("forward");
    geometry_msgs::PoseStamped person_pose;
    person_pose.header = current_customer_position_.header;
    person_pose.pose.position = current_customer_position_.point;
    person_pose.pose.orientation.w = 1.0;
    std::string target_id_result;
    geometry_msgs::PoseStamped target_pose_result;
    bool success = find_closest_target("glass", person_pose, false, target_id_result, target_pose_result);
    if(success)
    {
      // look at glass
      geometry_msgs::PointStamped target;
      target.header = target_pose_result.header;
      target.point = target_pose_result.pose.position;
      look_at_target("", target);

      state = [target_id_result](StateMachine* m) { m->state_pour(target_id_result); };
    }
    else
      state = &StateMachine::state_move_to_glass;
  }

  void state_pour(const std::string& glass_id)
  {
    ROS_INFO_STREAM("state_pour");
    publish_marker("state_pour");

    // temporary
    if(current_ingredients_.empty())
      state = &StateMachine::state_drink_finished;
    else
      state = &StateMachine::state_move_back_to_shelf;

    /*actionlib::SimpleActionClient<tiago_bartender_mtc::PourAction> client("tiago_bottle_pour", true);
    client.waitForServer();
    tiago_bartender_mtc::PourGoal goal;
    goal.container_id = glass_id;
    client.sendGoal(goal);
    while(!client.waitForResult(ros::Duration(5.0)))
      ROS_INFO("Waiting for bottle grasp result.");

    int res = client.getResult()->result.result;
    if(res == tiago_bartender_mtc::Result::SUCCESS)
    {
      ROS_INFO("Successfully poured.");
      if(current_ingredients_.empty())
        state = &StateMachine::state_drink_finished;
      else
        state = &StateMachine::state_move_back_to_shelf;
    }
    else if(res == tiago_bartender_mtc::Result::UNREACHABLE)
    {
      ROS_ERROR_STREAM("Could not reach glass for pouring.");
      // todo: reposition robot in different pose
      state = [glass_id](StateMachine* m) { m->state_pour(glass_id); };
    }
    else if(res == tiago_bartender_mtc::Result::NO_PLAN_FOUND || res == tiago_bartender_mtc::Result::EXECUTION_FAILED)
    {
      ROS_ERROR("Pouring failed.");
      state = [glass_id](StateMachine* m) { m->state_pour(glass_id); };
    }*/
  }

  void state_drink_finished()
  {
    ROS_INFO_STREAM("state_drink_finished");
    publish_marker("state_drink_finished");
    look_at_target("", last_person_position_);

    voice_command("enjoy");

    state = &StateMachine::state_move_back_to_shelf;
  }

  void state_move_back_to_shelf()
  {
    ROS_INFO_STREAM("state_move_back_to_shelf");
    publish_marker("state_move_back_to_shelf");
    look_at_target("forward");
    bool success = move_to_pose(last_bottle_pose_, false);
    if(success)
    {
      // look at bottle
      geometry_msgs::PointStamped target;
      target.header = last_bottle_pose_.header;
      target.point = last_bottle_pose_.pose.position;
      look_at_target("", target);

      state = &StateMachine::state_put_back_bottle;
    }
    else
      state = &StateMachine::state_move_back_to_shelf;
  }

  void state_put_back_bottle()
  {
    ROS_INFO_STREAM("state_put_back_bottle");
    publish_marker("state_put_back_bottle");
    ros::Duration(1).sleep();
    look_at_target("forward");

    // temporary
    if(current_ingredients_.empty())
      state = &StateMachine::state_idle_manager;
    else
      state = &StateMachine::state_move_to_bottle;

    /*actionlib::SimpleActionClient<tiago_bartender_mtc::PlaceAction> client("tiago_bottle_place", true);
    client.waitForServer();
    tiago_bartender_mtc::PlaceGoal goal;
    goal.place_pose = last_bottle_pose_;
    client.sendGoal(goal);
    while(!client.waitForResult(ros::Duration(5.0)))
      ROS_INFO("Waiting for bottle grasp result.");

    int res = client.getResult()->result.result;
    if(res == tiago_bartender_mtc::Result::SUCCESS)
    {
      ROS_INFO("Successfully picked.");
      if(current_ingredients_.empty())
        state = &StateMachine::state_idle_manager;
      else
        state = &StateMachine::state_move_to_bottle;
    }
    else if(res == tiago_bartender_mtc::Result::UNREACHABLE)
    {
      ROS_ERROR_STREAM("Could not reach glass for pouring.");
      // todo: reposition robot in different pose or give different place pose
      state = &StateMachine::state_put_back_bottle;
    }
    else if(res == tiago_bartender_mtc::Result::NO_PLAN_FOUND || res == tiago_bartender_mtc::Result::EXECUTION_FAILED)
    {
      ROS_ERROR_STREAM("Pouring failed.");
      state = &StateMachine::state_put_back_bottle;
    }*/
  }
 
  bool move_to_target(const std::string& target_name, bool look_at_target, geometry_msgs::PoseStamped& target_pose)
  {
    ROS_INFO("moving to target %s", target_name.c_str());
    tiago_bartender_navigation::MoveToTargetGoal goal;
    goal.target = target_name;
    goal.look_at_target = look_at_target;
    mtt_client_.sendGoal(goal);
    while(!mtt_client_.waitForResult(ros::Duration(5.0)))
      ROS_INFO("Waiting for move_to_target result.");
    if(mtt_client_.getState() == actionlib::SimpleClientGoalState::SUCCEEDED)
    {
      ROS_INFO("Successfully moved to target.");
      target_pose = mtt_client_.getResult()->target_pose_result;
      return true;
    }
    else
    {
      ROS_ERROR("Moving to target aborted.");
      return false;
    }
  }

  bool find_closest_target(const std::string& type_name, const geometry_msgs::PoseStamped& pose, const bool look_at_target, std::string& target_id_result, geometry_msgs::PoseStamped& target_pose)
  {
    ROS_INFO("Finding closest target of type %s", type_name.c_str());
    tiago_bartender_navigation::FindClosestTargetGoal goal;
    goal.target_type = type_name;
    goal.target_pose = pose;
    goal.look_at_target = look_at_target;
    fct_client_.sendGoal(goal);
    while(!fct_client_.waitForResult(ros::Duration(5.0)))
      ROS_INFO("Waiting for find_closest_target result.");
    if(fct_client_.getState() == actionlib::SimpleClientGoalState::SUCCEEDED)
    {
      ROS_INFO("Successfully found and moved to target.");
      auto result = fct_client_.getResult();
      target_id_result = result->target_id;
      target_pose = result->target_pose_result;
      return true;
    }
    else
    {
      ROS_ERROR("Find closest target aborted.");
      return false;
    }
  }

  bool move_to_pose(const geometry_msgs::PoseStamped pose, bool look_at_target)
  {
    tiago_bartender_navigation::MoveToTargetGoal goal;
    goal.target_pose = pose;
    goal.look_at_target = look_at_target;
    mtt_client_.sendGoal(goal);
    while(!mtt_client_.waitForResult(ros::Duration(5.0)))
      ROS_INFO("Waiting for move_to_target result.");
    if(mtt_client_.getState() == actionlib::SimpleClientGoalState::SUCCEEDED)
    {
      ROS_INFO("Successfully moved to target.");
      return true;
    }
    else
    {
      ROS_ERROR("Moving to target aborted.");
      return false;
    }
  }

  bool move_to_person()
  {
    tiago_bartender_navigation::MoveToTargetGoal goal;
    goal.target_pose.header = last_person_position_.header;
    goal.target_pose.pose.position = last_person_position_.point;
    goal.look_at_target = true;
    mtt_client_.sendGoal(goal);
    person_detected_ = false;

    while(!mtt_client_.waitForResult(ros::Duration(0.5)))
    {
      // update pose if person detected
      if(person_detected_)
      {
        goal.target_pose.header = last_person_position_.header;
        goal.target_pose.pose.position = last_person_position_.point;
        mtt_client_.sendGoal(goal);
        person_detected_ = false;
      }
    }
    if(mtt_client_.getState() == actionlib::SimpleClientGoalState::SUCCEEDED)
    {
      ROS_INFO("Successfully moved to target.");
      return true;
    }
    else
    {
      ROS_ERROR("Moving to target aborted.");
      return false;
    }
  }

  void publish_marker(const std::string& text)
  {
    marker_.text = text;
    marker_pub_.publish(marker_);
  }

  void extend_torso()
  {
    control_msgs::FollowJointTrajectoryGoal goal;
    trajectory_msgs::JointTrajectory torso_command;
    torso_command.joint_names.push_back("torso_lift_joint");
    trajectory_msgs::JointTrajectoryPoint jtp;
    jtp.positions.push_back(0.35);
    jtp.time_from_start = ros::Duration(0.5);
    torso_command.points.push_back(jtp);
    goal.trajectory = torso_command;

    fjt_client_.sendGoal(goal);
    while(!fjt_client_.waitForResult(ros::Duration(5.0)))
      ROS_INFO("Waiting to extend torso.");
    if(fjt_client_.getState() == actionlib::SimpleClientGoalState::SUCCEEDED)
      ROS_INFO("Successfully extended torso.");
  }

  void voice_command(std::string audio_id, bool wait_for_result = true)
  {
    tiago_bartender_speech::BartenderSpeechGoal speech_goal;
    speech_goal.id = audio_id;
    bs_client_.sendGoal(speech_goal);
    if(wait_for_result)
    {
      while(!bs_client_.waitForResult(ros::Duration(5.0)))
        ROS_INFO("Waiting for bartender speech action result.");
      if(bs_client_.getState() != actionlib::SimpleClientGoalState::SUCCEEDED)
        ROS_ERROR_STREAM("bartender speech action failed.");
    }
  }

  // move to a position in the middle of the bar
  void move_to_home()
  {
    ROS_INFO_STREAM("Moving to home pose.");
    move_base_msgs::MoveBaseGoal target;
    target.target_pose.header.frame_id = "map";
    target.target_pose.pose.position.x = -0.5;
    target.target_pose.pose.orientation.w = 1.0;
    movebase(target);
  }

  void movebase(move_base_msgs::MoveBaseGoal target)
  {
    mb_client_.sendGoal(target);
    mb_client_.waitForResult();
  }

  void look_at_target(std::string direction, geometry_msgs::PointStamped target_point = geometry_msgs::PointStamped())
  {
    // Point head to look at the customer
    tiago_bartender_behavior::LookAt srv;
    srv.request.direction = direction;
    srv.request.target_point = target_point;
    if (!look_at_client_.call(srv))
    {
      ROS_ERROR("Failed to call look_at_service");
    }
    ros::Duration(1.5).sleep();
  }

  double get_rand()
  {
    return unif_(rng_);
  }

  void person_detection_cb(const geometry_msgs::PointStamped::ConstPtr& msg)
  {
    last_person_position_ = *msg;
    person_detected_ = true;
  }

  std::function<void(StateMachine*)> state = &StateMachine::state_init;

  actionlib::SimpleActionClient<tiago_bartender_navigation::MoveToTargetAction> mtt_client_;
  actionlib::SimpleActionClient<tiago_bartender_navigation::FindClosestTargetAction> fct_client_;
  actionlib::SimpleActionClient<control_msgs::FollowJointTrajectoryAction> fjt_client_;
  actionlib::SimpleActionClient<tiago_bartender_speech::BartenderSpeechAction> bs_client_;
  actionlib::SimpleActionClient<tiago_bartender_menu::TakeOrderAction> to_client_;
  actionlib::SimpleActionClient<move_base_msgs::MoveBaseAction> mb_client_;

  ros::ServiceClient look_at_client_;

  // last position a person was detected in. gets updated by the person detection
  geometry_msgs::PointStamped last_person_position_;
  // static position of the customer. does not get updated anymore by the person detection
  geometry_msgs::PointStamped current_customer_position_;
  // pose of the object the robot moved to most recently
  geometry_msgs::PoseStamped last_bottle_pose_;
  ros::Publisher marker_pub_;
  ros::Subscriber person_detection_sub_;
  visualization_msgs::Marker marker_;
  ros::NodeHandle nh_;
  std::map<std::string, std::queue<std::pair<std::string, double>>> recipes_;
  std::queue<std::pair<std::string, double>> current_ingredients_;
  std::queue<geometry_msgs::PoseStamped> parked_bottle_poses_;


  // random number generation
  std::uniform_real_distribution<double> unif_;
  std::uniform_real_distribution<double> unif_idle_x_;
  std::uniform_real_distribution<double> unif_idle_y_;
  std::mt19937_64 rng_;

  bool person_detected_;
};

int main(int argc, char **argv)
{
  ros::init(argc, argv, "state_machine_node", ros::init_options::NoSigintHandler);
  ros::NodeHandle node;
  ros::AsyncSpinner spinner(4);
  spinner.start();

  StateMachine sm;
  sm.run();
}
