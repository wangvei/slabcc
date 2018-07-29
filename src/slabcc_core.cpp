// Copyright (c) 2018, University of Bremen, M. Farzalipour Tabriz
// Copyrights licensed under the 2-Clause BSD License.
// See the accompanying LICENSE.txt file for terms.

#include "slabcc_core.hpp"

extern slabcc_cell_type slabcc_cell;

mat dielectric_profiles(const rowvec2 &interfaces, const rowvec3 &diel_in, const rowvec3 &diel_out, const double &diel_erf_beta) {
	auto length = slabcc_cell.lengths(slabcc_cell.normal_direction);
	auto n_points = slabcc_cell.grid(slabcc_cell.normal_direction);
	const rowvec2 interfaces_cartesian = interfaces * length;
	sort(interfaces_cartesian);
	auto positions = linspace<rowvec>(0, length, n_points + 1);
	mat diels = zeros(n_points,3);
	const rowvec3 diel_sum = diel_in + diel_out;
	const rowvec3 diel_diff = diel_out - diel_in;

	for (uword k = 0; k < n_points; ++k) {
		double min_distance = 0;

		double diel_side = 1;
		// min distance of the point positions(k) to the interfaces in PBC [-length/2  length/2]
		const rowvec2 distances = fmod_p(positions(k) - interfaces_cartesian + length / 2, length) - length / 2;
		if (abs(distances(0)) < abs(distances(1))) {
			min_distance = distances(0);
			diel_side = -1;
		}
		else {
			min_distance = distances(1);
		}

		const double diel_edge = erf(min_distance / diel_erf_beta);
		diels.row(k) = (diel_diff * diel_side * diel_edge + diel_sum) / 2;
	}

	return diels;
}

void UpdateCell(const rowvec3& lengths, const urowvec3& divi) {
	slabcc_cell.lengths = lengths;
	slabcc_cell.grid = divi;
	slabcc_cell.voxel_vol = prod(lengths / divi);
}

cx_cube gaussian_charge(const double& Q, const rowvec3& pos, const double& sigma) {

	rowvec Gs = 2.0 * PI / slabcc_cell.lengths;
	rowvec Gx0 = regspace<rowvec>(ceil(-0.5 * slabcc_cell.grid(0)), ceil(0.5 * slabcc_cell.grid(0) - 1)) * Gs(0);
	rowvec Gy0 = regspace<rowvec>(ceil(-0.5 * slabcc_cell.grid(1)), ceil(0.5 * slabcc_cell.grid(1) - 1)) * Gs(1);
	rowvec Gz0 = regspace<rowvec>(ceil(-0.5 * slabcc_cell.grid(2)), ceil(0.5 * slabcc_cell.grid(2) - 1)) * Gs(2);
	cube Gx, Gy, Gz;
	tie(Gx, Gy, Gz) = ndgrid(Gx0, Gy0, Gz0);

	cube Gr = square(Gx) + square(Gy) + square(Gz);

	//real part of the complex charge
	//G(0)=q is for normalization
	cube rhok_real = Q * exp(-pow(sigma, 2) / 2 * Gr);

	cx_cube rhok = cx_cube(rhok_real, zeros(arma::size(rhok_real)));


	//  fourier shift theorem [intel compiler compatible form!] 
	rhok = rhok % exp(cx_cube(zeros<cube>(arma::size(Gx)), (Gx * pos(0) + Gy * pos(1) + Gz * pos(2)) * -1));

	return ifft(ifftshift(rhok)) / slabcc_cell.voxel_vol;
}

cx_cube poisson_solver_3D(const cx_cube &rho, mat diel) {
	auto length = slabcc_cell.lengths;
	auto n_points = slabcc_cell.grid;

	if (slabcc_cell.normal_direction != 2) {
		n_points.swap_cols(slabcc_cell.normal_direction, 2);
		length.swap_cols(slabcc_cell.normal_direction, 2);
		diel.swap_cols(slabcc_cell.normal_direction, 2);
	}

	const rowvec Gs = 2.0 * PI / length;

	rowvec Gx0 = ceil(regspace<rowvec>(-0.5 * n_points(0), 0.5 * n_points(0) - 1)) * Gs(0);
	rowvec Gy0 = ceil(regspace<rowvec>(-0.5 * n_points(1), 0.5 * n_points(1) - 1)) * Gs(1);
	rowvec Gz0 = ceil(regspace<rowvec>(-0.5 * n_points(2), 0.5 * n_points(2) - 1)) * Gs(2);

	Gx0 = ifftshift(Gx0);
	Gy0 = ifftshift(Gy0);
	Gz0 = ifftshift(Gz0);

	// 4PI is for the atomic units
	const auto rhok = fft(cx_cube(4.0 * PI * rho));
	auto Vk = cx_cube(arma::size(rhok));
	const cx_mat dielsG = fft(diel);

	cx_mat eps11 = circ_toeplitz(dielsG.col(0)) / Gz0.n_elem;
	cx_mat eps22 = circ_toeplitz(dielsG.col(1)) / Gz0.n_elem;
	cx_mat eps33 = circ_toeplitz(dielsG.col(2)) / Gz0.n_elem;
	mat GzGzp = Gz0.t() * Gz0;
	cx_mat Az = eps33 % GzGzp;
	cx_mat AG(arma::size(Az));

	for (uword k = 0; k < Gx0.n_elem; ++k) {
		for (uword m = 0; m < Gy0.n_elem; ++m) {
			vector<span> spans = { span(k), span(m), span() };
			swap(spans.at(slabcc_cell.normal_direction), spans.at(2));
			AG = Az + eps11 * pow(Gx0(k), 2) + eps22 * pow(Gy0(m), 2);

			if ((k == 0) && (m == 0)) {
				AG(0, 0) = 1;
			}
			Vk(spans.at(0), spans.at(1), spans.at(2)) = solve(AG, cx_colvec(vectorise(rhok(spans.at(0), spans.at(1), spans.at(2)))));
		}
	}

	// 0,0,0 in k-space corresponds to a constant in the real space: average potential over the supercell.
	Vk(0, 0, 0) = 0;
	cx_cube V = ifft(Vk);

	return V;
}

double potential_eval(const vector<double> &x, vector<double> &grad, void *slabcc_data)
{

	//input variables from the optimizer
	const rowvec2 interfaces = { x.at(0), x.at(1) };
	const uword variable_per_charge = 5;
	const uword defects = x.size() / variable_per_charge;
	rowvec sigma = zeros<rowvec>(defects);
	rowvec Qd = zeros<rowvec>(defects);
	mat defcenter(defects, 3, fill::zeros);

	for (uword i = 0; i < defects; ++i) {
		sigma(i) = x.at(2 + variable_per_charge * i);
		Qd(i) = x.at(3 + variable_per_charge * i);
		for (uword j = 0; j < 3; ++j) {
			defcenter(i, j) = x.at(4 + i * variable_per_charge + j);
		}
	}

	//input data
	const auto d = static_cast<opt_data *>(slabcc_data);
	const rowvec3 &diel_in = d->diel_in;
	const rowvec3 &diel_out = d->diel_out;
	const double &diel_erf_beta = d->diel_erf_beta;
	const cube &defect_potential = d->defect_potential;
	const rowvec3 &cell = slabcc_cell.lengths;
	const double volume = prod(cell);

	//output data
	mat& diels = d->diels;
	cx_cube& rhoM = d->rhoM;
	cx_cube& V = d->V;
	cube& V_diff = d->V_diff;
	double& initial_pot_MSE = d->initial_pot_MSE;

	diels = dielectric_profiles(interfaces, diel_in, diel_out, diel_erf_beta);
	
	rhoM.reset();
	rhoM = zeros<cx_cube>(as_size(slabcc_cell.grid));
	for (uword i = 0; i < Qd.n_elem; ++i) {
		rhoM += gaussian_charge(Qd(i), defcenter.row(i) % cell, sigma(i));
	}

	V = poisson_solver_3D(rhoM, diels);
	V_diff = real(V) * Hartree_to_eV - defect_potential;

	const double pot_MSE = accu(square(V_diff)) / V_diff.n_elem * 100;

	//if this is the 1st step, save the error for opt success checking
	if (initial_pot_MSE < 0) {
		initial_pot_MSE = pot_MSE;
	}

	if (is_active(verbosity::detailed_progress)) {
		cout << timing() << "-----------------------------------------" << endl;
		cout << timing() << "> shifted_interfaces=" << x.at(0) << " " << x.at(1) << endl;
		for (uword i = 0; i < defects; ++i) {
			cout << timing() << "> charge_sigma=" << sigma(i) << " charge_fraction="<< abs(Qd(i)/accu(Qd)) << endl;
			cout << timing() << "> shifted_charge_position=" << defcenter(i, 0) << " " << defcenter(i, 1) << " " << defcenter(i, 2) << endl;
		}
		cout << timing() << "Potential Mean Squared Error: " << pot_MSE << " %" << endl;
	}
	else {
		cout << ".";
	}

	return pot_MSE;
}

double do_optimize(const string& opt_algo, const double& opt_tol, const int &max_eval, const int &max_time, opt_data& opt_data, opt_vars& opt_vars, const bool &optimize_charge, const bool &optimize_interface) {
	double pot_MSE_min = 0;
	auto opt_algorithm = nlopt::LN_COBYLA;
	if (opt_algo == "BOBYQA") {
		if (opt_vars.Qd.n_elem == 1) {
			opt_algorithm = nlopt::LN_BOBYQA;
		}
		else {
			//BOBYQA in NLOPT 2.4.2 does not support the constraints!
			cout << timing() << "BOBYQA does not support the models with multiple charges! Will use COBYLA instead!" << endl;
		}
	}

	vector<double> opt_param, low_b, upp_b;
	tie(opt_param, low_b, upp_b) = optimizer_packer(opt_vars, optimize_charge, optimize_interface);
	nlopt::opt opt(opt_algorithm, opt_param.size());

	opt.set_lower_bounds(low_b);
	opt.set_upper_bounds(upp_b);

	opt.set_min_objective(potential_eval, &opt_data);
	opt.set_xtol_rel(opt_tol);   //tolerance for error value
	if (max_eval > 0) {
		opt.set_maxeval(max_eval);
	}
	if (max_time > 0) {
		opt.set_maxtime(60.0 * max_time);
	}
	if (opt_vars.Qd.n_elem > 1) {
		//add constraint to keep the total charge constant
		opt.add_equality_constraint(opt_charge_constraint, &opt_data, 1e-8);
	}

	if (is_active(verbosity::steps)) {
		const int var_per_charge = (opt_vars.Qd.n_elem == 1) ? 4 : 5;
		const int opt_parameters = opt_vars.Qd.n_elem * var_per_charge * optimize_charge + 2 * optimize_interface;
		cout << timing() << "Started optimizing " << opt_parameters << " parameters" << endl;
		cout << timing() << "NLOPT version: " << nlopt::version_major() << "." << nlopt::version_minor() << "." << nlopt::version_bugfix() << endl;
		cout << timing() << "Optimization algorithm: " << string(opt.get_algorithm_name()) << endl;
	}
	try {
		const nlopt::result nlopt_final_result = opt.optimize(opt_param, pot_MSE_min);

		if (nlopt_final_result == nlopt::MAXEVAL_REACHED) {
			cout << endl << timing() << ">> WARNING <<: optimization ended after "<< max_eval << " steps before reaching the requested accuracy!" << endl << endl;
		}
		else if (nlopt_final_result == nlopt::MAXTIME_REACHED) {
			cout << endl << timing() << ">> WARNING <<: optimization ended after " << max_time << " minutes of search before reaching the requested accuracy!" << endl << endl;
		}
	}
	catch (exception &e) {
		cerr << timing() << "Parameters optimization failed: " << e.what() << endl;
	}


	optimizer_unpacker(opt_param, opt_vars);
	//if we were just writing dots to the cout instead of the full progress in optimization
	if (!is_active(verbosity::detailed_progress)) { cout << endl; }
	if (is_active(verbosity::steps)) {
		cout << timing() << "Optimization ended." << endl;
	}

	return pot_MSE_min;
}

tuple<vector<double>, vector<double>, vector<double>> optimizer_packer(const opt_vars& opt_vars, const bool &optimize_charge, const bool &optimize_interface) {

	vector<double> opt_param = { opt_vars.interfaces(0), opt_vars.interfaces(1) };
	vector<double> low_b = { 0, 0 };		//lower bounds
	vector<double> upp_b = { 1, 1 };		//upper bounds
	if (!optimize_interface) {
		low_b = opt_param;
		upp_b = opt_param;
	}

	for (uword i = 0; i < opt_vars.charge_position.n_rows; ++i) {
		opt_param.push_back(opt_vars.sigma(i));
		opt_param.push_back(opt_vars.Qd(i));
		const auto min_charge = min(0.0, accu(opt_vars.Qd));
		const auto max_charge = max(0.0, accu(opt_vars.Qd));
		if (optimize_charge) {
			low_b.insert(low_b.end(), { 0.1, min_charge, 0, 0, 0 });	//sigma, q, x, y, z
			upp_b.insert(upp_b.end(), { 7, max_charge, 1, 1, 1 });
		}
		else {
			low_b.insert(low_b.end(), { opt_vars.sigma(i), opt_vars.Qd(i) });
			upp_b.insert(upp_b.end(), { opt_vars.sigma(i), opt_vars.Qd(i) });
		}


		for (uword j = 0; j < 3; ++j) {
			opt_param.push_back(opt_vars.charge_position(i, j));
			if (!optimize_charge) {
				low_b.push_back(opt_vars.charge_position(i, j));
				upp_b.push_back(opt_vars.charge_position(i, j));
			}
		}
	}

	//if we have only one charge, there's no need to optimize the Qd
	if (opt_vars.charge_position.is_rowvec()) {
		low_b.at(3) = opt_vars.Qd(0);
		upp_b.at(3) = opt_vars.Qd(0);
	}

	return make_tuple(opt_param, low_b, upp_b);
}

void optimizer_unpacker(const vector<double> &optimized_vars, opt_vars &opt_vars) {
	// the coefficients in this part depend on the set of our variables
	// they are ordered as: interfaces, :|[sigma, q, x, y, z]|:
	const uword variable_per_charge = 5;
	const uword n_charges = optimized_vars.size() / variable_per_charge;
	rowvec sigma = zeros<rowvec>(n_charges);
	rowvec Qd = zeros<rowvec>(n_charges);
	mat positions = zeros(n_charges, 3);
	opt_vars.interfaces = { optimized_vars.at(0), optimized_vars.at(1) };

	for (uword i = 0; i < n_charges; ++i) {
		opt_vars.sigma(i) = optimized_vars.at(2 + variable_per_charge * i);
		opt_vars.Qd(i) = optimized_vars.at(3 + variable_per_charge * i);
		for (uword j = 0; j < 3; ++j) {
			opt_vars.charge_position(i, j) = optimized_vars.at(4 + i * variable_per_charge + j);
		}
	}
}


void check_inputs(input_data input_set) {

	//all of these must be positive
	input_set.sigma = abs(input_set.sigma);
	input_set.max_eval = abs(input_set.max_eval);
	input_set.max_time = abs(input_set.max_time);
	input_set.interfaces = fmod_p(input_set.interfaces, 1);
	input_set.extrapol_grid_x = abs(input_set.extrapol_grid_x);
	input_set.opt_grid_x = abs(input_set.opt_grid_x);
	input_set.opt_tol = abs(input_set.opt_tol);

	if (input_set.diel_in.n_elem == 1) {
		input_set.diel_in = rowvec{ input_set.diel_in(0), input_set.diel_in(0), input_set.diel_in(0) };
	}

	if (input_set.diel_out.n_elem == 1) {
		input_set.diel_out = rowvec{ input_set.diel_out(0), input_set.diel_out(0), input_set.diel_out(0) };
	}

	 if (input_set.opt_tol > 1){
		input_set.opt_tol = 0.001;
		cout << endl << timing() << ">> WARNING <<: optimization tolerance is not defined properly" << endl;
		cout << timing() << "Will use optimize_tolerance=" << input_set.opt_tol << endl << endl;
	}

	if (input_set.sigma.n_elem != input_set.charge_position.n_rows) {
		input_set.sigma = rowvec(input_set.charge_position.n_rows, fill::ones);
		cout << endl << timing() << ">> WARNING <<: number of the defined sigma and charges does not match!" << endl;
		cout << timing() << "Will use sigma=" << input_set.sigma << endl << endl;
	}
	if (input_set.Qd.n_elem != input_set.sigma.n_elem) {
		input_set.Qd = rowvec(input_set.charge_position.n_rows, fill::ones);
		cout << endl << timing() << ">> WARNING <<: number of the charge_fraction and charges_sigma does not match!" << endl;
		cout << timing() << "Equal charge fractions will be assumed!" << endl << endl;

	}
	if (input_set.charge_position.n_cols != 3) {
		cerr << "ERROR: incorrect definition format for the charge positions!" << endl;
		cerr << "Positions should be defined as: charge_position = 0.1 0.2 0.3; 0.1 0.2 0.4;" << endl;
		exit(1);
	}

	if ((max(input_set.diel_in < 0)) || (max(input_set.diel_out < 0))) {
		cerr << "ERROR: dielectric tensor is not defined properly!" << endl;
		exit(1);
	}

	if (!file_exists(input_set.CHGCAR_NEU) || !file_exists(input_set.CHGCAR_CHG)
		|| !file_exists(input_set.LOCPOT_NEU) || !file_exists(input_set.LOCPOT_CHG)) {
		cerr << "ERROR: One or more of the input files could not be found!" << endl;
		exit(1);
	}

	if ((input_set.opt_algo != "BOBYQA") && (input_set.opt_algo != "COBYLA")) {
		input_set.opt_algo = "COBYLA";
		cout << endl << timing() << ">> WARNING <<: Unknown optimization algorithm is selected! Will use "<< input_set.opt_algo << " instead!" << endl << endl;
		
	}

	if (input_set.extrapol_steps_num < 3) {
		cout << endl << timing() << ">> WARNING <<: Extrapolation cannot be done with steps < 3" << endl << endl;
		input_set.extrapol_steps_num = 3;
	}

	if (is_active(verbosity::detailed_progress)) {
		cout << timing() << "Input parameters verified!" << endl;
	}

}

void parse_input_params(const string& input_file, ofstream& output_fstream, const input_data& input_set) {

	INIReader reader(input_file);

	if (reader.ParseError() < 0) {
		cerr << "Cannot load '" << input_file << "'" << endl;
		exit(1);
	}

	verbos = reader.GetInteger("verbosity", 0);
	input_set.CHGCAR_NEU = reader.GetStr("CHGCAR_neutral", "CHGCAR.N");
	input_set.LOCPOT_NEU = reader.GetStr("LOCPOT_neutral", "LOCPOT.N");
	input_set.LOCPOT_CHG = reader.GetStr("LOCPOT_charged", "LOCPOT.C");
	input_set.CHGCAR_CHG = reader.GetStr("CHGCAR_charged", "CHGCAR.C");
	input_set.charge_position = reader.GetMat("charge_position", {});
	input_set.Qd = reader.GetVec("charge_fraction", rowvec(input_set.charge_position.n_rows, fill::ones) / input_set.charge_position.n_rows);
	input_set.sigma = reader.GetVec("charge_sigma", rowvec(input_set.charge_position.n_rows, fill::ones));
	input_set.slabcenter = reader.GetVec("slab_center", { 0.5, 0.5, 0.5 });
	input_set.normal_direction = xyz2int(reader.GetStr("normal_direction", "z"));
	input_set.interfaces = reader.GetVec("interfaces", { 0.25, 0.75 });
	input_set.diel_in = reader.GetVec("diel_in", { 1 });
	input_set.diel_out = reader.GetVec("diel_out", { 1 });
	input_set.diel_erf_beta = reader.GetReal("diel_taper", 1);
	input_set.optimize_charge = reader.GetBoolean("optimize_charge", true);
	input_set.optimize_interface = reader.GetBoolean("optimize_interfaces", true);
	input_set.opt_algo = reader.GetStr("optimize_algorithm", "COBYLA");
	input_set.opt_tol = reader.GetReal("optimize_tolerance", 1e-3);
	input_set.max_eval = reader.GetInteger("optimize_maxsteps", 0);
	input_set.max_time = reader.GetInteger("optimize_maxtime", 0);
	input_set.opt_grid_x = reader.GetReal("optimize_grid_x", 0.8);
	input_set.extrapol_grid_x = reader.GetReal("extrapolate_grid_x", 1);
	input_set.extrapol_steps_num = reader.GetInteger("extrapolate_steps_number", 4);
	input_set.extrapol_steps_size = reader.GetReal("extrapolate_steps_size", 0.5);
	//input_set.extrapol_slab = reader.GetBoolean("extrapolate_slab", true);

	reader.dump_parsed(output_fstream);

	slabcc_cell.normal_direction = input_set.normal_direction;
}

double opt_charge_constraint(const vector<double> &x, vector<double> &grad, void *data)
{
	const int variable_per_charge = 5;
	const int n_charges = x.size() / variable_per_charge;
	rowvec Qd = zeros<rowvec>(n_charges);
	for (uword i = 0; i < n_charges; ++i) {
		Qd(i) = x.at(3 + variable_per_charge * i);
	}

	const auto d = static_cast<const opt_data *>(data);
	const auto constraint = accu(Qd) - d->Q0;
	if (is_active(verbosity::detailed_progress)) {
		cout << timing() << "Total charge error : " << abs(constraint) << endl;
	}
	return constraint;
}

tuple <rowvec, rowvec> extrapolate_3D(const int &extrapol_steps_num, const double &extrapol_steps_size, const rowvec3 &diel_in, const rowvec3 &diel_out, const rowvec2 &interfaces, const double &diel_erf_beta, const mat &charge_position, const rowvec &Qd, const rowvec &sigma, const double &grid_multiplier) {

	const uword normal_direction = slabcc_cell.normal_direction;
	rowvec Es = zeros<rowvec>(extrapol_steps_num - 1), sizes = Es;
	const rowvec cell0 = slabcc_cell.lengths;
	const urowvec grid0 = slabcc_cell.grid;
	const rowvec3 grid_ext = grid_multiplier * conv_to<rowvec>::from(grid0);
	const urowvec3 grid_ext_u = { (uword)grid_ext(0),(uword)grid_ext(1),(uword)grid_ext(2) };
	for (int n = 0; n < extrapol_steps_num - 1; ++n) {

		const double extrapol_factor = 1 + extrapol_steps_size * (1.0 + n);

		UpdateCell(cell0 * extrapol_factor, grid_ext_u);

		//extrapolated interfaces
		rowvec2 interfaces_ext = interfaces;
		uvec interface_sorted_i = sort_index(interfaces);
		interfaces_ext(interface_sorted_i(1)) += abs(interfaces(0) - interfaces(1)) * (extrapol_factor - 1);
		interfaces_ext /= extrapol_factor;

		//charges each moved to the same distance from its original nearest interface
		mat charge_position_shifted = charge_position / extrapol_factor;

		for (auto charge = 0; charge < charge_position.n_rows; ++charge) {
			const rowvec2 distance_to_interfaces = abs(charge_position(charge, normal_direction) - interfaces);
			if (distance_to_interfaces(0) < distance_to_interfaces(1)) {
				charge_position_shifted(charge, normal_direction) += interfaces_ext(0) - interfaces(0) / extrapol_factor;
			}
			else {
				charge_position_shifted(charge, normal_direction) += interfaces_ext(1) - interfaces(1) / extrapol_factor;
			}
		}

		mat diels = dielectric_profiles(interfaces_ext, diel_in, diel_out, diel_erf_beta);

		cx_cube rhoM(as_size(slabcc_cell.grid), fill::zeros);
		for (uword i = 0; i < charge_position.n_rows; ++i) {
			rhoM += gaussian_charge(Qd(i), (charge_position_shifted.row(i) % slabcc_cell.lengths), sigma(i));
		}
		auto Q = accu(real(rhoM)) * slabcc_cell.voxel_vol;

		rhoM -= Q / prod(slabcc_cell.lengths);
		auto V = poisson_solver_3D(rhoM, diels);
		double EperModel = 0.5 * accu(real(V % rhoM)) * slabcc_cell.voxel_vol * Hartree_to_eV;

		if (is_active(verbosity::steps)) {
			cout << timing() << extrapol_factor << "\t\t" << EperModel << "\t" << interfaces_ext(0) * slabcc_cell.lengths(normal_direction) << "\t" << interfaces_ext(1) * slabcc_cell.lengths(normal_direction) << "\t" << charge_position_shifted(0, slabcc_cell.normal_direction) * slabcc_cell.lengths(slabcc_cell.normal_direction) << endl;
		}
		Es(n) = EperModel;
		sizes(n) = 1.0 / extrapol_factor;

	}

	return make_tuple(Es, sizes);
}

tuple <rowvec, rowvec> extrapolate_2D(const int &extrapol_steps_num, const double &extrapol_steps_size, const rowvec3 &diel_in, const rowvec3 &diel_out, const rowvec2 &interfaces, const double &diel_erf_beta, const mat &charge_position, const rowvec &Qd, const rowvec &sigma, const double &grid_multiplier) {

	const uword normal_direction = slabcc_cell.normal_direction;
	rowvec Es = zeros<rowvec>(extrapol_steps_num - 1), sizes = Es;
	const rowvec cell0 = slabcc_cell.lengths;
	const urowvec grid0 = slabcc_cell.grid;
	const rowvec3 grid_ext = grid_multiplier * conv_to<rowvec>::from(grid0);
	const urowvec3 grid_ext_u = { (uword)grid_ext(0),(uword)grid_ext(1),(uword)grid_ext(2) };
	UpdateCell(cell0, grid_ext_u);
	mat diels0 = dielectric_profiles(interfaces, diel_in, diel_out, diel_erf_beta);

	for (int n = 0; n < extrapol_steps_num - 1; ++n) {

		const double extrapol_factor = 1 + extrapol_steps_size * (1.0 + n);
		UpdateCell(cell0 * extrapol_factor, grid_ext_u);
		//extrapolated interfaces
		rowvec2 interfaces_ext = interfaces/ extrapol_factor;

		const mat charge_position_ext = charge_position / extrapol_factor;
		mat diels = dielectric_profiles(interfaces_ext, diel_in, diel_out, diel_erf_beta);

		cx_cube rhoM(as_size(slabcc_cell.grid), fill::zeros);
		for (uword i = 0; i < charge_position.n_rows; ++i) {
			rhoM += gaussian_charge(Qd(i), (charge_position_ext.row(i) % slabcc_cell.lengths), sigma(i));
		}
		const auto Q = accu(real(rhoM)) * slabcc_cell.voxel_vol;
		rhoM -= Q / prod(slabcc_cell.lengths);
		auto V = poisson_solver_3D(rhoM, diels);
		double EperModel = 0.5 * accu(real(V % rhoM)) * slabcc_cell.voxel_vol * Hartree_to_eV;

		if (is_active(verbosity::steps)) {
			cout << timing() << extrapol_factor << "\t\t" << EperModel << "\t" << interfaces_ext(0) * slabcc_cell.lengths(normal_direction) << "\t" << interfaces_ext(1) * slabcc_cell.lengths(normal_direction) << "\t" << charge_position_ext(0, slabcc_cell.normal_direction) * slabcc_cell.lengths(slabcc_cell.normal_direction) << endl;
		}
		Es(n) = EperModel;
		sizes(n) = 1.0 / extrapol_factor;

	}

	return make_tuple(Es, sizes);
}


vector<double> nonlinear_fit(const double& opt_tol, nonlinear_fit_data& fit_data) {
	double fit_MSE = 0;
	vector<double> fit_parameters = { 1, 1, 1, 1};
	const auto opt_algorithm = nlopt::LN_COBYLA;

	nlopt::opt opt(opt_algorithm, 4);

	opt.set_min_objective(fit_eval, &fit_data);
	opt.set_xtol_rel(opt_tol);   //tolerance for error value

	try {
		const nlopt::result nlopt_final_result = opt.optimize(fit_parameters, fit_MSE);
	}
	catch (exception &e) {
		cerr << timing() << "Nonlinear fitting failed: " << e.what() << endl;
	}

	return fit_parameters;
}

double fit_eval(const vector<double> &c, vector<double> &grad, void *data)
{
	const auto d = static_cast<const nonlinear_fit_data *>(data);
	const rowvec scales = d ->sizes;
	const rowvec energies = d->energies;
	const double madelung_term = d->madelung_term;
	const rowvec model_energies = c.at(0) + c.at(1) * scales + c.at(2) * square(scales) + (c.at(1) - madelung_term) / c.at(3) * exp(-c.at(3) * scales);
	const double fit_MSE = accu(square(energies - model_energies));
	//cout << "MSE: " << fit_MSE << endl;
	return fit_MSE;
}

void check_cells(supercell& Neutral_supercell, supercell& Charged_supercell, input_data input_set) {

	// all positive vectors
	Neutral_supercell.cell_vectors = abs(Neutral_supercell.cell_vectors);
	Charged_supercell.cell_vectors = abs(Charged_supercell.cell_vectors);

	// equal cell size
	if (!approx_equal(Neutral_supercell.cell_vectors * Neutral_supercell.scaling,
		Charged_supercell.cell_vectors * Charged_supercell.scaling, "reldiff", 0.001)) {
		cerr << "ERROR: Size vectors of the input files does not match!" << endl;
		exit(1);
	}

	// cell needs rotation or is not orthogonal
	vec cellvec_nonzeros = nonzeros(Neutral_supercell.cell_vectors);
	if (cellvec_nonzeros.n_elem != 3) {
		cerr << "ERROR: unsupported supercell shape!" << endl;
		exit(1);
	}

	// cell just needs axis swap
	if (!approx_equal(diagvec(Neutral_supercell.cell_vectors * Neutral_supercell.scaling),
		nonzeros(Neutral_supercell.cell_vectors * Charged_supercell.scaling), "reldiff", 0.001)) {
		uvec indexes = find(Neutral_supercell.cell_vectors.t());
		Mat<uword> subs = ind2sub(size(3, 3), indexes);
		// TODO: more human brain must be used here
		for (uword i = 0; i < 3; ++i) {
			if (subs(0, i) != subs(1, i)) {
				if (is_active(verbosity::detailed_progress)) {
					cout << timing() << "File axis swaped!" << endl;
				}
				swap_axes(Neutral_supercell, subs(0, i), subs(1, i));
				swap_axes(Charged_supercell, subs(0, i), subs(1, i));
				input_set.charge_position.swap_cols(subs(0, i), subs(1, i));
				input_set.diel_in.swap_cols(subs(0, i), subs(1, i));
				input_set.diel_out.swap_cols(subs(0, i), subs(1, i));
				input_set.slabcenter.swap_cols(subs(0, i), subs(1, i));
				
				if (slabcc_cell.normal_direction == subs(0, i)) {
					slabcc_cell.normal_direction = subs(1, i);
				}else if (slabcc_cell.normal_direction == subs(1, i)) {
					slabcc_cell.normal_direction = subs(0, i);
				}

				indexes = find(Neutral_supercell.cell_vectors.t());
				subs = ind2sub(size(3, 3), indexes);
				i = 0;
			}
		}
	}

	// equal grid
	const SizeCube input_grid = arma::size(Neutral_supercell.potential);
	if ((input_grid != arma::size(Charged_supercell.potential)) ||
		(input_grid != arma::size(Charged_supercell.charge)) ||
		(input_grid != arma::size(Neutral_supercell.charge))) {

		cerr << "ERROR: Data grids of the CHGCAR or LOCPOT files does not match!" << endl;
		exit(1);
	}

	if (is_active(verbosity::detailed_progress)) {
		cout << timing() << "Files loaded and cross-checked!" << endl;
	}
}