//
//  pfilter.cpp
//  EpiGenMCMC
//
//  Created by Lucy Li on 05/05/2016.
//  Copyright (c) 2016 Lucy Li, Imperial College London. All rights reserved.
//

#include "pfilter.h"
#include <iostream>



namespace EpiGenPfilter {
    double pfilter(Model & sim_model, Parameter & model_params, MCMCoptions & options, Particle &particles, Trajectory & output_traj, TimeSeriesData &epi_data, TreeData &tree_data, MultiTreeData &multitree_data) {
        double loglik = 0.0;
        int num_groups = options.num_groups;
        int num_particles = options.particles;
        int init_seed = options.seed;
        int total_dt = options.total_dt;
        double sim_dt = options.sim_dt;
        int total_steps = ceil((double)total_dt/(double)options.pfilter_every);
        int add_dt = 0;
        double ESS_threshold = options.pfilter_threshold*(double)num_particles;
        Likelihood likelihood_calc;
//        std::vector <Parameter> values;// (options.num_threads, model_params);
//        for (int i=0; i!=options.num_threads; ++i) values.push_back(model_params);
//        for (int i=0; i!=model_params.get_total_params(); ++i) values.push_back(model_params.get(i));
        std::vector <std::vector<double> > values(options.num_threads, std::vector<double>(model_params.get_total_params(), 0.0));
        for (int i=0; i!=options.num_threads; ++i) {
            for (int j=0; j!=model_params.get_total_params(); ++j) {
                values[i][j] = model_params.get(j);
            }
        }
//        printf("Size of values = %d\n",values.size());
        double reporting_rate = 1.0;
        if (model_params.param_exists("reporting")) {
            reporting_rate = model_params.get("reporting");
        }
        std::vector <std::string> param_names = model_params.get_names_vector();
        if (model_params.param_exists("time_before_data")) {
            add_dt = model_params.get("time_before_data");
        }
        if (options.save_traj) {
            if (add_dt > 0) {
                particles.start_particle_tracing(add_dt+total_dt, num_groups);
            }
            else if (add_dt < 0) {
                particles.start_particle_tracing(add_dt+total_dt, num_groups);
                total_steps = ceil((double)(total_dt+add_dt)/(double)options.pfilter_every);
            }
            else {
                particles.start_particle_tracing(total_dt, num_groups);
            }
        }
        // Simulate model and calculate likelihood assuming no observed data
        if (model_params.param_exists("time_before_data")) {
            if (add_dt > 0) {
                omp_set_num_threads(options.num_threads);
//                std::vector <Trajectory *> curr_trajs;
//                for (int i=0; i!=num_particles; ++i) {
//                    curr_trajs.push_back(particles.get_traj(i));
//                }
#pragma omp parallel for shared(particles, values) schedule(static, 1)
                for (int tn=0; tn<options.num_threads; tn++) {
                    for (int i=tn; i<num_particles; i+=options.num_threads) {
                        // Adjust length of trajectory
                        particles.get_traj(i)->resize(add_dt, num_groups);
                        sim_model.simulate(values[tn], param_names, particles.get_traj(i), 0, add_dt, sim_dt, total_dt, options.rng[tn]);
                        if (options.which_likelihood<2) {
                            double w = likelihood_calc.binomial_lik(reporting_rate, particles.get_traj(i)->get_total_traj(), add_dt+total_dt, 0, add_dt, num_groups, false);
                            particles.set_weight(w, i, false);
                        }
                        if (options.save_traj) {
                            particles.save_traj_to_matrix(i, 0, add_dt);
                            particles.save_ancestry(i, 0, add_dt);
                        }
                    }
                }
            }
        }
        init_seed += num_particles;
        int t=0;
        int start_dt;
        int end_dt;
        for (t=0; t!=total_steps; ++t) {
            start_dt = t*options.pfilter_every;
            end_dt = std::min(total_dt, (t+1)*options.pfilter_every);
            if (add_dt < 0) {
                start_dt -= add_dt;
                end_dt = std::min(total_dt, end_dt-add_dt);
            }
            omp_set_num_threads(options.num_threads);
//            std::vector <Trajectory *> curr_trajs;
//            for (int i=0; i!=num_particles; ++i) {
//                curr_trajs.push_back(particles.get_traj(i));
//            }
#pragma omp parallel for shared (particles, values) schedule(static,1)
            for (int tn=0; tn<options.num_threads; tn++) {
                for (int i=tn; i<num_particles; i+=options.num_threads) {
                    // Adjust length of trajectory
                    particles.get_traj(i)->resize(end_dt-start_dt, options.num_groups);
                    sim_model.simulate(values[tn], param_names, particles.get_traj(i), start_dt, end_dt, sim_dt, total_dt, options.rng[tn]);
                    double w = 1.0;
                    if (options.which_likelihood<2) {
                        double A = particles.get_traj(i)->get_total_traj();
//                        double B = particles.get_traj(i)->get_state(0);
//                        if (i==0) {
//                            std::cout << "current traj: " << A << "\tcurrent prevalence: " << B << std::endl;
//                        }
                        w *= likelihood_calc.binomial_lik(reporting_rate, A, epi_data.get_data_ptr(0), add_dt+total_dt, start_dt, end_dt, add_dt, options.num_groups, false);
                    }
                    if (options.which_likelihood != 1) {
                        w *= likelihood_calc.coalescent_lik(particles.get_traj(i)->get_traj2_ptr(0), tree_data.get_binomial_ptr(0), tree_data.get_interval_ptr(0), tree_data.get_ends_ptr(0), start_dt, end_dt, add_dt, false);
                    }
                    particles.set_weight(w, i, true);
                    if (options.save_traj) {
                        particles.save_traj_to_matrix(i, start_dt+add_dt, end_dt+add_dt);
                        particles.save_ancestry(i, start_dt+add_dt, end_dt+add_dt);
                    }
                }
            }
            double curr_ESS = particles.get_ESS();
            if (curr_ESS < ESS_threshold) {
                double total_weight = particles.get_total_weight();
                if (total_weight == 0.0) {
                    loglik = -0.1*std::numeric_limits<double>::max();
                    break;
                }
                loglik += log(total_weight) - log(num_particles);
                particles.resample(options.rng[0]);
            }
            else {
                particles.reset_parents();
            }
        }
        if (options.save_traj) {
            output_traj.resize((total_dt+add_dt), num_groups);
            if (loglik > -0.1*std::numeric_limits<double>::max()) {
                particles.retrace_traj(output_traj, options.rng[0]);
            }
        }
        for (int i=0; i!=num_particles; ++i) {
            particles.get_traj(i)->reset();
        }
        std::vector < std::vector<double>>().swap(values);
        return (loglik);
    }
}