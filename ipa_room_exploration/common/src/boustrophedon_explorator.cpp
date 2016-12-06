#include <ipa_room_exploration/boustrophedon_explorator.h>

// Constructor
boustrophedonExplorer::boustrophedonExplorer()
{

}

// Function that creates a room exploration path for the given map, by using the morse cellular decomposition method proposed in
//
// "H. Choset, E. Acar, A. A. Rizzi and J. Luntz,
// "Exact cellular decompositions in terms of critical points of Morse functions," Robotics and Automation, 2000. Proceedings.
// ICRA '00. IEEE International Conference on, San Francisco, CA, 2000, pp. 2270-2277 vol.3."
//
// This method takes the given map and separates it into several cells. Each cell is obstacle free and so allows an
// easier path planning. For each cell then a boustrophedon path is planned, which goes up, down and parallel to the
// upper and lower boundaries of the cell, see the referenced paper for details. This function does the following steps:
// I.	Sweep a slice (a morse function) trough the given map and check for connectivity of this line,
//		i.e. how many connected segments there are. If the connectivity increases, i.e. more segments appear,
//		an IN event occurs that opens new separate cells, if it decreases, i.e. segments merge, an OUT event occurs that
//		merges two cells together. If an event occurs, the algorithm checks along the current line for critical points,
//		that are points that trigger the events. From these the boundary of the cells are drawn, starting from the CP
//		and going left/right until a black pixel is hit.
// II.	After all cells have been determined by the sweeping slice, the algorithm finds these by using cv::findContours().
//		This gives a set of points for each cell, that are used to create a generalizedPolygon out of each cell.
// III.	After all polygons have been created, plan the path trough all of them for the field of view s.t. the whole area
//		is covered. To do so, first a global path trough all cells is generated, using the traveling salesmen problem
//		formulation. This produces an optimal visiting order of the cells. Next for each cell a boustrophedon path is
//		determined, which goes back and forth trough the cell and between the horizontal paths along the boundaries of
//		the cell, what ensures that the whole area of the cell is covered. The startpoint of the cell-path is determined
//		by the endpoint of the previous cell, s.t. the distance between two cell-paths is minimized.
// IV.	The previous step produces a path for the field of view. If wanted this path gets mapped to the robot path s.t.
//		the field of view follows the wanted path. To do so simply a vector transformation is applied. If the computed robot
//		pose is not in the free space, another accessible point is generated by finding it on the radius around the fow
//		middlepoint s.t. the distance to the last robot position is minimized. If this is not wanted one has to set the
//		corresponding Boolean to false (shows that the path planning should be done for the robot footprint).
void boustrophedonExplorer::getExplorationPath(const cv::Mat& room_map, std::vector<geometry_msgs::Pose2D>& path,
		const float map_resolution, const cv::Point starting_position, const cv::Point2d map_origin,
		const float fitting_circle_radius, const int path_eps, const bool plan_for_footprint,
		const Eigen::Matrix<float, 2, 1> robot_to_fow_vector)
{
	ROS_INFO("Planning the boustrophedon path trough the room.");
	// *********************** I. Sweep a slice trough the map and mark the found cell boundaries. ***********************
	// create a map copy to mark the cell boundaries
	cv::Mat cell_map = room_map.clone();

	// find smallest y-value for that a white pixel occurs, to set initial y value and find initial number of segments
	size_t y_start = 0;
	int n_start = 0;
	bool found = false, obstacle = false;
	for(size_t y=0; y<room_map.rows; ++y)
	{
		for(size_t x=0; x<room_map.cols; ++x)
		{
			if(room_map.at<uchar>(y,x) == 255 && found == false)
			{
				y_start = y;
				found = true;
			}
			else if(found == true && obstacle == false && room_map.at<uchar>(y,x) == 0)
			{
				++n_start;
				obstacle = true;
			}
			else if(found == true && obstacle == true && room_map.at<uchar>(y,x) == 255)
			{
				obstacle = false;
			}
		}

		if(found == true)
			break;
	}

	// swipe trough the map and detect critical points
	int previous_number_of_segments = n_start;
	for(size_t y=y_start+1; y<room_map.rows; ++y) // start at y_start+1 because we know number of segments at y_start
	{
		int number_of_segments = 0; // int to count how many segments at the current slice are
		bool obstacle_hit = false; // bool to check if the line currently hit an obstacle, s.t. not all black pixels trigger an event
		bool hit_white_pixel = false; // bool to check if a white pixel has been hit at the current slice, to start the slice at the first white pixel

		for(size_t x=0; x<room_map.cols; ++x)
		{
			if(room_map.at<uchar>(y,x) == 255 && hit_white_pixel == false)
				hit_white_pixel = true;

			else if(hit_white_pixel == true)
			{
				if(obstacle_hit == false && room_map.at<uchar>(y,x) == 0) // check for obstacle
				{
					++number_of_segments;
					obstacle_hit = true;
				}
				else if(obstacle_hit == true && room_map.at<uchar>(y,x) == 255) // check for leaving obstacle
				{
					obstacle_hit = false;
				}
			}
		}

		// reset hit_white_pixel to use this Boolean later
		hit_white_pixel = false;

		// check if number of segments has changed --> event occurred
		if(previous_number_of_segments < number_of_segments) // IN event
		{
			// check the current slice again for critical points
			for(int x=0; x<room_map.cols; ++x)
			{
				if(room_map.at<uchar>(y,x) == 255 && hit_white_pixel == false)
					hit_white_pixel = true;

				else if(hit_white_pixel == true && room_map.at<uchar>(y,x) == 0)
				{
					// check over black pixel for other black pixels, if none occur a critical point is found
					bool critical_point = true;
					for(int dx=-1; dx<=1; ++dx)
						if(room_map.at<uchar>(y-1,x+dx) == 0)
							critical_point = false;

					// if a critical point is found mark the separation, note that this algorithm goes left and right
					// starting at the critical point until an obstacle is hit, because this prevents unnecessary cells
					// behind other obstacles on the same y-value as the critical point
					if(critical_point == true)
					{
						// to the left until a black pixel is hit
						for(int dx=-1; x+dx>0; --dx)
						{
							if(cell_map.at<uchar>(y,x+dx) == 255)
								cell_map.at<uchar>(y,x+dx) = 0;
							else if(cell_map.at<uchar>(y,x+dx) == 0)
								break;
						}

						// to the right until a black pixel is hit
						for(int dx=1; x+dx<room_map.cols; ++dx)
						{
							if(cell_map.at<uchar>(y,x+dx) == 255)
								cell_map.at<uchar>(y,x+dx) = 0;
							else if(cell_map.at<uchar>(y,x+dx) == 0)
								break;
						}
					}
				}
			}
		}
		else if(previous_number_of_segments > number_of_segments) // OUT event
		{
			// check the previous slice again for critical points --> y-1
			for(int x=0; x<room_map.cols; ++x)
			{
				if(room_map.at<uchar>(y-1,x) == 255 && hit_white_pixel == false)
					hit_white_pixel = true;

				else if(hit_white_pixel == true && room_map.at<uchar>(y-1,x) == 0)
				{
					// check over black pixel for other black pixels, if none occur a critical point is found
					bool critical_point = true;
					for(int dx=-1; dx<=1; ++dx)
						if(room_map.at<uchar>(y,x+dx) == 0) // check at side after obstacle
							critical_point = false;

					// if a critical point is found mark the separation, note that this algorithm goes left and right
					// starting at the critical point until an obstacle is hit, because this prevents unnecessary cells
					// behind other obstacles on the same y-value as the critical point
					if(critical_point == true)
					{
						// to the left until a black pixel is hit
						for(int dx=-1; x+dx>0; --dx)
						{
							if(cell_map.at<uchar>(y-1,x+dx) == 255)
								cell_map.at<uchar>(y-1,x+dx) = 0;
							else if(cell_map.at<uchar>(y-1,x+dx) == 0)
								break;
						}

						// to the right until a black pixel is hit
						for(int dx=1; x+dx<room_map.cols; ++dx)
						{
							if(cell_map.at<uchar>(y-1,x+dx) == 255)
								cell_map.at<uchar>(y-1,x+dx) = 0;
							else if(cell_map.at<uchar>(y-1,x+dx) == 0)
								break;
						}
					}
				}
			}
		}

		// save the found number of segments
		previous_number_of_segments = number_of_segments;
	}
//	cv::imshow("cells", cell_map);
//	cv::waitKey();

	// *********************** II. Find the separated cells. ***********************
	std::vector<std::vector<cv::Point> > cells;
	cv::findContours(cell_map, cells, CV_RETR_EXTERNAL, CV_CHAIN_APPROX_SIMPLE);
//	 testing
//	cv::Mat black_map = cv::Mat(cell_map.rows, cell_map.cols, cell_map.type(), cv::Scalar(0));
//	for(size_t i=0; i<cells.size(); ++i)
//	{
//		for(size_t j=0; j<cells[i].size(); ++j)
//		{
//			cv::circle(black_map, cells[i][j], 2, cv::Scalar(127), CV_FILLED);
//			cv::imshow("contours", black_map);
//			cv::waitKey();
//		}
//	}

	// create generalized Polygons out of the contours to handle the cells
	std::vector<generalizedPolygon> cell_polygons;
	std::vector<cv::Point> polygon_centers;
	for(size_t cell=0; cell<cells.size(); ++cell)
	{
		generalizedPolygon current_cell(cells[cell]);
		cell_polygons.push_back(current_cell);
		polygon_centers.push_back(current_cell.getCenter());
	}

	ROS_INFO("Found the cells in the given map.");

	// *********************** III. Determine the cell paths. ***********************
	// determine the start cell that contains the start position
	int min_index = 0;
	cv::Point starting_point(starting_position.x, starting_position.y); // conversion of Pose2D to cv::Point for convenience

//	testing
//	cv::Mat center_map = room_map.clone();
//	for(size_t i=0; i<cell_polygons.size(); ++i)
//		cv::circle(center_map, cell_polygons[i].getCenter(), 2, cv::Scalar(127), CV_FILLED);
//	cv::circle(center_map, starting_point, 4, cv::Scalar(100), CV_FILLED);
//	cv::imshow("centers", center_map);
//	cv::waitKey();

	for(std::vector<generalizedPolygon>::iterator cell = cell_polygons.begin(); cell != cell_polygons.end(); ++cell)
		if(cv::pointPolygonTest(cell->getVertexes(), starting_point, false) >= 0)
			min_index = cell - cell_polygons.begin();

	// determine the optimal visiting order of the cells
	ConcordeTSPSolver tsp_solver;
	std::vector<int> optimal_order = tsp_solver.solveConcordeTSP(room_map, polygon_centers, 0.25, 0.0, map_resolution, min_index, 0);

	// go trough the cells and determine the boustrophedon paths
	ROS_INFO("Starting to get the paths for each cell, number of cells: %d", (int)cell_polygons.size());
	int fow_radius_as_int = (int) std::floor(fitting_circle_radius); // convert fow-radius to int
	cv::Point robot_pos = starting_point; // Point that keeps track of the last point after the boustrophedon path in each cell
	std::vector<cv::Point> fow_middlepoint_path;
	for(size_t cell=0; cell<cell_polygons.size(); ++cell)
	{
		// access current cell
		generalizedPolygon current_cell = cell_polygons[optimal_order[cell]];

		// get a map that has only the current cell drawn in
		cv::Mat current_cell_map;
		current_cell.drawPolygon(current_cell_map, cv::Scalar(127));

		// get the min/max x/y values for this cell
		int min_x, max_x, min_y, max_y;
		current_cell.getMinMaxCoordinates(min_x, max_x, min_y, max_y);

		// get the left and right edges of the path
		std::vector<boustrophedonHorizontalLine> path_lines;
		int y, dx;

		// check where to start the planning of the path (if the cell is smaller that the fow_diameter start in the middle of it)
		if((max_y - min_y) <= 2.0*fow_radius_as_int)
			y = min_y + 0.5 * (max_y - min_y);
		else
			y = (min_y-1) + fow_radius_as_int;
		do
		{
			boustrophedonHorizontalLine current_line;
			cv::Point left_edge, right_edge;
			// check the for the current row to the right/left for the first white pixel (valid position), starting from
			// the minimal/maximal x value
			dx = min_x;
			bool found = false;
			// get the leftmost edges of the path
			do
			{
				if(room_map.at<uchar>(y, dx) == 255)
				{
					left_edge = cv::Point(dx+fow_radius_as_int, y); // add fow radius s.t. the edge is not on the wall
					found = true;
				}
				else
					++dx;
			}while(dx < current_cell_map.cols && found == false); // dx < ... --> safety, should never hold

			// get the rightmost edges of the path
			dx = max_x;
			found = false;
			do
			{
				if(room_map.at<uchar>(y, dx) == 255)
				{
					right_edge = cv::Point(dx-fow_radius_as_int, y); // subtract fow radius s.t. edge is not on the wall
					found = true;
				}
				else
					--dx;
			}while(dx > 0 && found == false);

			// save found horizontal line
			current_line.left_edge_ = left_edge;
			current_line.right_edge_ = right_edge;
			path_lines.push_back(current_line);

			// increase y by given fow-radius value
			y += fow_radius_as_int;
		}while(y <= max_y);

//		testing
//		for(size_t i=0; i<path_lines.size(); ++i)
//		{
//			cv::line(current_cell_map, path_lines[i].left_edge_, path_lines[i].right_edge_, cv::Scalar(200), 1);
//			cv::circle(current_cell_map, path_lines[i].left_edge_, 2, cv::Scalar(200), CV_FILLED);
//			cv::circle(current_cell_map, path_lines[i].right_edge_, 2, cv::Scalar(200), CV_FILLED);
//			cv::imshow("path edges", current_cell_map);
//			cv::waitKey();
//		}

		// get the edge nearest to the current robot position to start the boustrophedon path at by looking at the
		// upper and lower horizontal path (possible nearest locations)
		bool start_from_upper_path = true;
		bool left = true; // Boolean to determine on which side the path should start and to check where the path ended
		double dist1 = path_planner_.planPath(room_map, robot_pos, path_lines[0].left_edge_, 1.0, 0.0, map_resolution);
		double dist2 = path_planner_.planPath(room_map, robot_pos, path_lines[0].right_edge_, 1.0, 0.0, map_resolution);
		double dist3= path_planner_.planPath(room_map, robot_pos, path_lines.back().left_edge_, 1.0, 0.0, map_resolution);
		double dist4 = path_planner_.planPath(room_map, robot_pos, path_lines.back().right_edge_, 1.0, 0.0, map_resolution);

		if((dist3 < dist1 && dist3 < dist2) || (dist4 < dist1 && dist4 < dist2)) // start on lower line
		{
			start_from_upper_path = false;
			if(dist4 < dist3)
				left = false;
		}
		else
			if(dist2 < dist1)
				left = false;

		// calculate the points between the edge points and create the boustrophedon path with this
		bool start = true;
		if(start_from_upper_path == true) // plan the path starting from upper horizontal line
		{
			for(std::vector<boustrophedonHorizontalLine>::iterator line=path_lines.begin(); line!=path_lines.end(); ++line)
			{
				if(start == true) // at the beginning of path planning start at first horizontal line --> no vertical points between lines
				{
					if(left == true)
						robot_pos = line->left_edge_;
					else
						robot_pos = line->right_edge_;
					start = false;
				}

				if(left == true) // plan path to left and then to right edge
				{
					// get points between horizontal lines by using the Astar-path
					std::vector<cv::Point> astar_path;
					path_planner_.planPath(room_map, robot_pos, line->left_edge_, 1.0, 0.0, map_resolution, 0, &astar_path);
					for(size_t path_point=0; path_point<astar_path.size(); ++path_point)
					{
						if(cv::norm(robot_pos - astar_path[path_point]) >= path_eps)
						{
							fow_middlepoint_path.push_back(astar_path[path_point]);
							robot_pos = astar_path[path_point];
						}
					}
					fow_middlepoint_path.push_back(line->left_edge_);

					// get points between left and right edge
					int dx = path_eps;
					while((line->left_edge_.x+dx) < line->right_edge_.x)
					{
						fow_middlepoint_path.push_back(cv::Point(line->left_edge_.x+dx, line->left_edge_.y));
						dx += path_eps;
					}
					fow_middlepoint_path.push_back(line->right_edge_);

					// set robot position to right
					robot_pos = line->right_edge_;
					left = false;
				}
				else // plan path to right then to left edge
				{
					// get points between horizontal lines
					std::vector<cv::Point> astar_path;
					path_planner_.planPath(room_map, robot_pos, line->right_edge_, 1.0, 0.0, map_resolution, 0, &astar_path);
					for(size_t path_point=0; path_point<astar_path.size(); ++path_point)
					{
						if(cv::norm(robot_pos - astar_path[path_point]) >= path_eps)
						{
							fow_middlepoint_path.push_back(astar_path[path_point]);
							robot_pos = astar_path[path_point];
						}
					}
					fow_middlepoint_path.push_back(line->right_edge_);

					// get points between left and right edge
					int dx = -path_eps;
					while((line->right_edge_.x+dx) > line->left_edge_.x)
					{
						fow_middlepoint_path.push_back(cv::Point(line->right_edge_.x+dx, line->right_edge_.y));
						dx -= path_eps;
					}
					fow_middlepoint_path.push_back(line->left_edge_);

					// set robot position to right
					robot_pos = line->left_edge_;
					left = true;
				}
			}
		}
		else // plan the path from the lower horizontal line
		{
			for(std::vector<boustrophedonHorizontalLine>::reverse_iterator line=path_lines.rbegin(); line!=path_lines.rend(); ++line)
			{
				if(start == true) // at the beginning of path planning start at first horizontal line --> no vertical points between lines
				{
					if(left == true)
						robot_pos = line->left_edge_;
					else
						robot_pos = line->right_edge_;
					start = false;
				}

				if(left == true) // plan path to left and then to right edge
				{
					// get points between horizontal lines
					std::vector<cv::Point> astar_path;
					path_planner_.planPath(room_map, robot_pos, line->left_edge_, 1.0, 0.0, map_resolution, 0, &astar_path);
					for(size_t path_point=0; path_point<astar_path.size(); ++path_point)
					{
						if(cv::norm(robot_pos - astar_path[path_point]) >= path_eps)
						{
							fow_middlepoint_path.push_back(astar_path[path_point]);
							robot_pos = astar_path[path_point];
						}
					}
					fow_middlepoint_path.push_back(line->left_edge_);

					// get points between left and right edge
					int dx = path_eps;
					while((line->left_edge_.x+dx) < line->right_edge_.x)
					{
						fow_middlepoint_path.push_back(cv::Point(line->left_edge_.x+dx, line->left_edge_.y));
						dx += path_eps;
					}
					fow_middlepoint_path.push_back(line->right_edge_);

					// set robot position to right
					robot_pos = line->right_edge_;
					left = false;
				}
				else // plan path to right then to left edge
				{
					// get points between horizontal lines
					std::vector<cv::Point> astar_path;
					path_planner_.planPath(room_map, robot_pos, line->right_edge_, 1.0, 0.0, map_resolution, 0, &astar_path);
					for(size_t path_point=0; path_point<astar_path.size(); ++path_point)
					{
						if(cv::norm(robot_pos - astar_path[path_point]) >= path_eps)
						{
							fow_middlepoint_path.push_back(astar_path[path_point]);
							robot_pos = astar_path[path_point];
						}
					}
					fow_middlepoint_path.push_back(line->right_edge_);

					// get points between left and right edge
					int dx = -path_eps;
					while((line->right_edge_.x+dx) > line->left_edge_.x)
					{
						fow_middlepoint_path.push_back(cv::Point(line->right_edge_.x+dx, line->right_edge_.y));
						dx -= path_eps;
					}
					fow_middlepoint_path.push_back(line->left_edge_);

					// set robot position to right
					robot_pos = line->left_edge_;
					left = true;
				}
			}
		}
	}
//		testing
//		std::cout << "printing path" << std::endl;
//		cv::Mat fow_path_map = room_map.clone();
//		for(size_t i=0; i<fow_middlepoint_path.size(); ++i)
//			cv::circle(fow_path_map, fow_middlepoint_path[i], 2, cv::Scalar(200), CV_FILLED);
////		for(size_t i=0; i<optimal_order.size()-1; ++i)
////			cv::line(fow_path_map, polygon_centers[optimal_order[i]], polygon_centers[optimal_order[i+1]], cv::Scalar(100), 1);
//		cv::imshow("cell path", fow_path_map);
//		cv::waitKey();

	// create poses with an angle
	std::vector<geometry_msgs::Pose2D> fow_poses;
	for(unsigned int point_index=0; point_index<fow_middlepoint_path.size(); ++point_index)
	{
		// get the vector from the current point to the next point
		cv::Point current_point = fow_middlepoint_path[point_index];
		cv::Point next_point = fow_middlepoint_path[(point_index+1)%(fow_middlepoint_path.size())];
		cv::Point vector = next_point - current_point;

		float angle = std::atan2(vector.y, vector.x);

		// add the next navigation goal to the path
		geometry_msgs::Pose2D current_pose;
		current_pose.x = current_point.x;
		current_pose.y = current_point.y;
		current_pose.theta = angle;

		fow_poses.push_back(current_pose);
	}

	ROS_INFO("Found the cell paths.");

	// if the path should be planned for the robot footprint create the path and return here
	if(plan_for_footprint == true)
	{
		for(std::vector<geometry_msgs::Pose2D>::iterator pose=fow_poses.begin(); pose != fow_poses.end(); ++pose)
		{
			geometry_msgs::Pose2D current_pose;
			current_pose.x = (pose->x * map_resolution) + map_origin.x;
			current_pose.y = (pose->y * map_resolution) + map_origin.y;
			current_pose.theta = pose->theta;
			path.push_back(current_pose);
		}
		return;
	}

	// *********************** IV. Get the robot path out of the fow path. ***********************
	// go trough all computed fow poses and compute the corresponding robot pose
	ROS_INFO("Starting to map from field of view pose to robot pose");
	mapPath(room_map, path, fow_poses, robot_to_fow_vector, map_resolution, map_origin, starting_position);
}
